#include "test_fixtures.hpp"

#include "anyhttp/h1/io.hpp"
#include "anyhttp/net.hpp"

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#if ANYHTTP_ASIO
#include <boost/asio/ssl/host_name_verification.hpp>
#endif

#include <nghttp2/nghttp2.h>

#include <array>
#include <chrono>
#include <concepts>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace testing;

namespace http = boost::beast::http;
namespace h1 = anyhttp::beast_impl::h1;

// =================================================================================================

//
// How the server ends a connection, over HTTP/1.1 and HTTP/2, in cleartext and over TLS.
//
// A clean end has each side send its GOAWAY (HTTP/2 only), its "close_notify" (TLS only) and its
// FIN, in this order, and keep reading until the peer's FIN before it closes the socket: closing
// earlier makes the kernel answer whatever still arrives with an RST, which may cost the peer what
// it has not read yet. See docs/capture.md, "What a clean end looks like".
//
// The client side is driven by hand, nghttp2 for HTTP/2 included, so that the tests see what goes
// over the wire. It ends the way the server does, with one constraint over TLS: it sends its
// close_notify only once the server has nothing left to send. Waiting for the peer's close_notify
// while application data still arrives is something the TLS libraries disagree about: AWS-LC fails
// the shutdown, OpenSSL drops the data.
//
// What ends the stream is the server's FIN in cleartext, and its close_notify over TLS, where the
// FIN is looked for in the socket beneath. Never in the same place twice: an ASIO socket that has
// reported the end of the stream once completes the next read only on the next epoll event, which
// may never come.
//

// -------------------------------------------------------------------------------------------------

/// A bare nghttp2 client session: output() is what it has to send, input() takes what arrived.
class H2Client
{
public:
   struct Goaway
   {
      int32_t last_stream_id;
      uint32_t error_code;
   };

   H2Client()
   {
      nghttp2_session_callbacks* callbacks;
      nghttp2_session_callbacks_new(&callbacks);
      nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
      nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close);
      nghttp2_session_client_new(&session_, callbacks, this);
      nghttp2_session_callbacks_del(callbacks);
      nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, nullptr, 0);
   }

   ~H2Client() { nghttp2_session_del(session_); }
   H2Client(const H2Client&) = delete;
   H2Client& operator=(const H2Client&) = delete;

   /// Submits a GET without a body and returns its stream ID.
   int32_t submit_get(std::string_view scheme, std::string_view authority, std::string_view path)
   {
      std::array nva{nv(":method", "GET"), nv(":scheme", scheme), nv(":authority", authority),
                     nv(":path", path)};
      auto id =
         nghttp2_submit_request2(session_, nullptr, nva.data(), nva.size(), nullptr, nullptr);
      EXPECT_GT(id, 0) << nghttp2_strerror(id);
      return id;
   }

   /// Submits a GOAWAY without an error.
   void submit_goaway()
   {
      auto rv = nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, 0, NGHTTP2_NO_ERROR, nullptr, 0);
      EXPECT_EQ(rv, 0) << nghttp2_strerror(rv);
   }

   std::string output()
   {
      std::string out; // nghttp2 starts with the client magic by itself
      const uint8_t* data;
      while (auto n = nghttp2_session_mem_send2(session_, &data))
      {
         EXPECT_GT(n, 0) << nghttp2_strerror(n);
         if (n < 0)
            break;
         out.append(reinterpret_cast<const char*>(data), n);
      }
      return out;
   }

   /**
    * Feeds \p data to the session, and looks for the server's GOAWAY in it by hand: nghttp2 reports
    * nothing more once it has sent a GOAWAY of its own and has no stream left, and the server's
    * GOAWAY comes after the client's.
    */
   void input(std::span<const uint8_t> data)
   {
      auto n = nghttp2_session_mem_recv2(session_, data.data(), data.size());
      EXPECT_EQ(n, static_cast<nghttp2_ssize>(data.size())) << nghttp2_strerror(n);

      // A frame header is 9 bytes: length (24 bits), type, flags, stream ID (RFC 9113, 4.1).
      frames_.insert(frames_.end(), data.begin(), data.end());
      while (frames_.size() >= 9)
      {
         const auto length = static_cast<size_t>(frames_[0] << 16 | frames_[1] << 8 | frames_[2]);
         if (frames_.size() < 9 + length)
            break;
         if (frames_[3] == NGHTTP2_GOAWAY && length >= 8)
            goaway =
               Goaway{.last_stream_id = static_cast<int32_t>(read32(&frames_[9]) & 0x7fffffff),
                      .error_code = read32(&frames_[13])};
         frames_.erase(frames_.begin(), frames_.begin() + 9 + length);
      }
   }

   std::optional<Goaway> goaway; ///< the server's GOAWAY, once it has arrived
   std::map<int32_t, unsigned> status; ///< the ":status" of each response
   std::set<int32_t> closed; ///< the streams that have been closed

private:
   static nghttp2_nv nv(std::string_view name, std::string_view value)
   {
      return {const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name.data())),
              const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value.data())), name.size(),
              value.size(), NGHTTP2_NV_FLAG_NONE};
   }

   static uint32_t read32(const uint8_t* p)
   {
      return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
             static_cast<uint32_t>(p[2]) << 8 | p[3];
   }

   static int on_header(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name,
                        size_t namelen, const uint8_t* value, size_t valuelen, uint8_t,
                        void* user_data)
   {
      if (std::string_view(reinterpret_cast<const char*>(name), namelen) == ":status")
         static_cast<H2Client*>(user_data)->status[frame->hd.stream_id] =
            std::stoul(std::string(reinterpret_cast<const char*>(value), valuelen));
      return 0;
   }

   static int on_stream_close(nghttp2_session*, int32_t stream_id, uint32_t, void* user_data)
   {
      static_cast<H2Client*>(user_data)->closed.insert(stream_id);
      return 0;
   }

   nghttp2_session* session_ = nullptr;
   std::vector<uint8_t> frames_; ///< what has arrived of the frame that is incomplete yet
};

// -------------------------------------------------------------------------------------------------

/// The socket beneath \p stream, which shows the FIN or the RST that TLS hides.
inline TcpSocket& socket_of(TcpSocket& socket) { return socket; }

inline TcpSocket& socket_of(TlsStream& stream)
{
#if ANYHTTP_COROSIO
   return stream.socket();
#else
   return stream.next_layer();
#endif
}

// -------------------------------------------------------------------------------------------------

/// What the tests do, over a plain TCP socket or over TLS, as \p Stream says.
template <typename Stream>
class ConnectionEndBase : public Server
{
protected:
   static constexpr bool tls = std::same_as<Stream, TlsStream>;

   /// Connects, and has the server speak the protocol of the test.
   virtual Task<Stream> connect() = 0;

   Task<TcpSocket> connect_tcp()
   {
      auto socket = io::make_socket(context.get_executor());
      check(co_await io::connect(socket, {server->local_endpoint()}));
      co_return socket;
   }

   bool h2() const { return GetParam() == Protocol::h2; }
   std::string authority() const { return std::format("127.0.0.2:{}", port()); }

   /// One GET, answered with 200. Over HTTP/1.1, \p connection is what the request asks for.
   Task<void> get(Stream& stream, std::string_view connection = "keep-alive")
   {
      if (h2())
      {
         auto id = h2_.submit_get(tls ? "https" : "http", authority(), "/dump");
         EXPECT_FALSE(co_await flush(stream));
         std::array<uint8_t, 16 * 1024> data;
         while (!h2_.closed.contains(id))
         {
            auto [ec, n] = co_await read_some(stream, asio::buffer(data));
            if (ec)
               throw_error(ec);
            h2_.input({data.data(), n});
            EXPECT_FALSE(co_await flush(stream)); // the SETTINGS ACK
         }
         EXPECT_EQ(h2_.status[id], 200);
      }
      else
      {
         http::request<http::empty_body> request{http::verb::get, "/dump", 11};
         request.set(http::field::host, authority());
         request.set(http::field::connection, connection);
         check(co_await h1::write_message(stream, request));

         http::response<http::string_body> response;
         check(co_await h1::read_message(stream, buffer_, response));
         EXPECT_EQ(response.result_int(), 200);
         EXPECT_EQ(response.keep_alive(), connection == "keep-alive");
      }
   }

   /// Asks the server to end the connection: a last request over HTTP/1.1, a GOAWAY over HTTP/2.
   Task<void> ask_to_end(Stream& stream)
   {
      if (h2())
      {
         h2_.submit_goaway();
         EXPECT_FALSE(co_await flush(stream));
      }
      else
         co_await get(stream, "close");
   }

   /// Writes what the HTTP/2 session has to send, if anything.
   Task<error_code> flush(Stream& stream)
   {
      auto out = h2_.output();
      if (out.empty())
         co_return error_code{};
      auto [ec, n] = co_await io::write(stream, asio::buffer(out));
      co_return ec;
   }

   /**
    * Reads what the server still sends, up to the end of \p stream, and returns how it ended. Over
    * HTTP/2, that is where the server's GOAWAY comes; over HTTP/1.1, nothing may come. The end is
    * the server's close_notify over TLS, its FIN otherwise.
    */
   Task<error_code> read_end(Stream& stream)
   {
      EXPECT_EQ(buffer_.size(), 0) << "unread data left over from the response";

      std::array<uint8_t, 16 * 1024> data;
      for (;;)
      {
         auto [ec, n] = co_await read_some(stream, asio::buffer(data));
         if (ec)
            co_return ec;
         if (h2())
            h2_.input({data.data(), n});
         else
            ADD_FAILURE() << std::format("{} bytes after the last response", n);
      }
   }

   /// Checks that the server's GOAWAY has arrived, says nothing went wrong, and covers stream 1.
   void expect_goaway()
   {
      if (!h2())
         return;
      EXPECT_TRUE(h2_.goaway) << "the server ended without a GOAWAY";
      if (h2_.goaway)
      {
         EXPECT_EQ(h2_.goaway->error_code, NGHTTP2_NO_ERROR);
         EXPECT_EQ(h2_.goaway->last_stream_id, 1);
      }
   }

   /// Ends the client's side: its close_notify over TLS, which must be answered, then its FIN.
   Task<void> end(Stream& stream)
   {
      if constexpr (tls)
         EXPECT_FALSE(co_await close_notify(stream)) << "close_notify not answered";
      EXPECT_FALSE(io::shutdown(stream, io::Shutdown::send));
   }

   /**
    * Sends the client's close_notify and waits for the server's. By hand, and not through
    * async_teardown(): that is how the server ends its side, and the client must not break along
    * with it.
    */
   Task<error_code> close_notify(Stream& stream)
      requires tls
   {
      Timer timer(context.get_executor());
      timer.arm(timeout, [&stream] { io::cancel(stream); });
#if ANYHTTP_COROSIO
      auto [ec] = co_await stream.tls().shutdown();
#else
      auto [ec] = co_await stream.async_shutdown(asio::as_tuple);
#endif
      timer.cancel();
      co_return ec;
   }

   /**
    * Reads from \p stream, giving up after a while: a server that does not end the connection, or
    * an ASIO socket read once more after its end (see above), must fail a test, not hang it.
    */
   template <typename S>
   Task<std::tuple<error_code, size_t>> read_some(S& stream, asio::mutable_buffer buffer)
   {
      Timer timer(context.get_executor());
      timer.arm(timeout, [&stream] { io::cancel(stream); });
      auto [ec, n] = co_await io::read_some(stream, buffer);
      timer.cancel();
      co_return std::tuple{ec, n};
   }

   static constexpr auto timeout = 5s;

   /**
    * Reads the socket beneath the TLS stream, after its end, and returns how that ended: with the
    * server's FIN, that is \c eof, and with nothing before it. An RST is \c connection_reset.
    */
   Task<error_code> read_fin(Stream& stream)
      requires tls
   {
      std::array<char, 1024> data;
      for (;;)
      {
         auto [ec, n] = co_await read_some(socket_of(stream), asio::buffer(data));
         if (ec)
            co_return ec;
         ADD_FAILURE() << std::format("{} bytes after the end of the stream", n);
      }
   }

   /// Runs \p task to completion, then stops the server.
   void run(Task<void> task)
   {
      launch(context.get_executor(), std::move(task), [this](const std::exception_ptr& ep) {
         if (ep)
            ADD_FAILURE() << what(ep);
         server.reset();
      });
      Server::run();
   }

   // ----------------------------------------------------------------------------------------------

   /// The client asks for the end, and the server ends first, in order and without delay.
   Task<void> client_asks_to_end()
   {
      auto stream = co_await connect();
      co_await get(stream);

      const auto start = std::chrono::steady_clock::now();
      co_await ask_to_end(stream);
      EXPECT_EQ(co_await read_end(stream), errors::eof);
      expect_goaway();

      co_await end(stream);
      if constexpr (tls)
         EXPECT_EQ(co_await read_fin(stream), errors::eof);
      EXPECT_LT(std::chrono::steady_clock::now() - start, 1s) << "ran into a timeout";
   }

   /// The client ends its side first, and the server follows, without delay.
   Task<void> client_ends_first()
   {
      auto stream = co_await connect();
      co_await get(stream);

      const auto start = std::chrono::steady_clock::now();
      co_await end(stream);
      if constexpr (tls)
         EXPECT_EQ(co_await read_fin(stream), errors::eof);
      else
      {
         EXPECT_EQ(co_await read_end(stream), errors::eof);
         expect_goaway();
      }
      EXPECT_LT(std::chrono::steady_clock::now() - start, 1s) << "ran into a timeout";
   }

   /// The client keeps sending after the server's FIN, which must not be answered with an RST.
   Task<void> data_after_the_end()
   {
      auto stream = co_await connect();
      co_await get(stream);

      co_await ask_to_end(stream);
      EXPECT_EQ(co_await read_end(stream), errors::eof);
      expect_goaway();
      if constexpr (tls)
      {
         EXPECT_FALSE(co_await close_notify(stream)) << "close_notify not answered";
         EXPECT_EQ(co_await read_fin(stream), errors::eof);
      }

      //
      // A server that closed its socket right after its FIN answers the first of these with an
      // RST, which fails the second one. What the data is does not matter: it is never read.
      //
      auto& socket = socket_of(stream);
      const std::string late(1000, 'x');
      for (int i = 0; i < 2; ++i)
      {
         auto [ec, n] = co_await io::write(socket, asio::buffer(late));
         EXPECT_FALSE(ec) << std::format("write #{}: {}", i + 1, ec.message());
         co_await delay(100ms);
      }
      EXPECT_FALSE(io::shutdown(socket, io::Shutdown::send));
   }

   H2Client h2_;
   boost::beast::flat_buffer buffer_;
};

// =================================================================================================

class ConnectionEnd : public ConnectionEndBase<TcpSocket>
{
protected:
   Task<TcpSocket> connect() override { co_return co_await connect_tcp(); }
};

INSTANTIATE_TEST_SUITE_P(ConnectionEnd, ConnectionEnd,
                         Values(anyhttp::Protocol::h1, anyhttp::Protocol::h2), NameGenerator);

TEST_P(ConnectionEnd, WHEN_client_asks_to_end_THEN_server_ends_in_order)
{
   run(client_asks_to_end());
}

TEST_P(ConnectionEnd, WHEN_client_ends_first_THEN_server_follows) { run(client_ends_first()); }

TEST_P(ConnectionEnd, WHEN_data_arrives_after_the_end_THEN_server_drains_it_instead_of_resetting)
{
   run(data_after_the_end());
}

// =================================================================================================

class TlsConnectionEnd : public ConnectionEndBase<TlsStream>
{
protected:
   /// TLS 1.3, with the certificate checked against the test PKI's root CA, for 127.0.0.2, and
   /// ALPN asking for the protocol of the test.
   Task<TlsStream> connect() override
   {
      auto stream = io::make_tls_stream(co_await connect_tcp(), tls_);
#if ANYHTTP_COROSIO
      stream.tls().set_hostname("127.0.0.2");
#else
      stream.set_verify_callback(asio::ssl::host_name_verification("127.0.0.2"));
#endif
      check(co_await io::handshake(stream, Role::client));
      EXPECT_EQ(io::alpn(stream), h2() ? "h2" : "http/1.1");
      co_return stream;
   }

#if ANYHTTP_COROSIO
   static TlsContext make_context(Protocol protocol)
   {
      namespace corosio = boost::corosio;
      corosio::tls_context context;
      throw_on_error(context.set_min_protocol_version(corosio::tls_version::tls_1_3));
      throw_on_error(context.load_verify_file("pki/out/root.pem"));
      throw_on_error(context.set_verify_mode(corosio::tls_verify_mode::peer));
      throw_on_error(context.set_alpn({protocol == Protocol::h2 ? "h2" : "http/1.1"}));
      return context;
   }

   static void throw_on_error(error_code ec)
   {
      if (ec)
         throw_error(ec);
   }
#else
   static TlsContext make_context(Protocol protocol)
   {
      asio::ssl::context context{asio::ssl::context::tlsv13};
      context.load_verify_file("pki/out/root.pem");
      context.set_verify_mode(asio::ssl::verify_peer);
      const std::string_view alpn = protocol == Protocol::h2 ? "\x02h2" : "\x08http/1.1";
      EXPECT_EQ(SSL_CTX_set_alpn_protos(context.native_handle(),
                                        reinterpret_cast<const unsigned char*>(alpn.data()),
                                        alpn.size()),
                0); // sic: 0 is success here
      return context;
   }
#endif

   TlsContext tls_ = make_context(GetParam());
};

INSTANTIATE_TEST_SUITE_P(TlsConnectionEnd, TlsConnectionEnd,
                         Values(anyhttp::Protocol::h1, anyhttp::Protocol::h2), NameGenerator);

TEST_P(TlsConnectionEnd, WHEN_client_asks_to_end_THEN_server_ends_in_order)
{
   run(client_asks_to_end());
}

TEST_P(TlsConnectionEnd, WHEN_client_ends_first_THEN_server_follows)
{
   //
   // The server sends its GOAWAY after the client's close_notify, which is where the TLS libraries
   // part ways (see above). A client is to send its own GOAWAY first anyway (RFC 9113, section
   // 6.8), and then it is the server that ends first.
   //
   if (h2())
      GTEST_SKIP() << "an HTTP/2 client ends with a GOAWAY, not with its close_notify";
   run(client_ends_first());
}

TEST_P(TlsConnectionEnd, WHEN_data_arrives_after_the_end_THEN_server_drains_it_instead_of_resetting)
{
   run(data_after_the_end());
}

//
// The server waits for the client's close_notify before its FIN, but not for good.
//
TEST_P(TlsConnectionEnd, WHEN_client_never_answers_the_close_notify_THEN_server_ends_anyway)
{
   run([&]() -> Task<void> {
      auto stream = co_await connect();
      co_await get(stream);

      co_await ask_to_end(stream);
      EXPECT_EQ(co_await read_end(stream), errors::eof);
      expect_goaway();

      const auto start = std::chrono::steady_clock::now();
      EXPECT_EQ(co_await read_fin(stream), errors::eof);
      EXPECT_LT(std::chrono::steady_clock::now() - start, 3s);

      EXPECT_FALSE(io::shutdown(stream, io::Shutdown::send));
   }());
}

// =================================================================================================
