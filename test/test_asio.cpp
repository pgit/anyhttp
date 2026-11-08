//
// The completion-token spelling of the API, which only ASIO has: async_submit(), async_shutdown()
// and the rest. Underneath, each of them is the operation the coroutine spelling runs, which the
// rest of the suite tests; what is tested here is what the token layer adds -- that the initiations
// work with the tokens a caller would use, a plain callback included, and give cancel_after what it
// needs, an executor for its timer. Empty in a COROSIO build.
//
#include "anyhttp/config.hpp"

#if ANYHTTP_ASIO

#include "test_fixtures.hpp"

#include "anyhttp/net.hpp"
#include "anyhttp/quic.hpp"

#include <boost/asio/cancel_after.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

using namespace testing;

namespace
{

// =================================================================================================

/// A timeout for cancel_after() that no operation here comes near: it must never fire.
constexpr auto never = 10s;

/// Rethrows what a coroutine spawned by a test threw, failing the test.
void rethrow(const std::exception_ptr& ep)
{
   if (ep)
      std::rethrow_exception(ep);
}

/// Reads \p reader to its end, with async_read_some() and \p token.
template <typename Token>
Task<std::string> read_to_end(Reader& reader, Token token)
{
   std::string body;
   std::array<char, 1024> buffer;
   for (;;)
   {
      auto [ec, n] = co_await reader.async_read_some(boost::asio::buffer(buffer), token);
      body.append(buffer.data(), n);
      if (ec == errors::eof)
         co_return body;
      EXPECT_FALSE(ec) << ec.message();
      if (ec)
         co_return body;
   }
}

/// The server's side of an exchange: echoes the request body, with \p token on every operation.
template <typename Token>
Task<void> echo(server::Request request, server::Response response, Token token)
{
   auto body = co_await read_to_end(request, token);
   auto [submit_ec] = co_await response.async_submit(200, {}, token);
   EXPECT_FALSE(submit_ec) << submit_ec.message();
   auto [write_ec, written] = co_await response.async_write(boost::asio::buffer(body), token);
   EXPECT_FALSE(write_ec) << write_ec.message();
   //
   // Not checked: over HTTP/3, the server's writes complete when the peer acknowledges them, and a
   // client that has read all of the response may close the connection before it acknowledges the
   // last of it.
   //
   std::ignore = co_await response.async_write_eof(boost::asio::buffer("!", 1), token);
}

/// The client's side of an exchange, with \p token on every operation, ending with a shutdown.
template <typename Token>
Task<void> exchange(Session& session, boost::urls::url url, Token token)
{
   auto [submit_ec, request] = co_await session.async_submit(url, {}, token);
   EXPECT_FALSE(submit_ec) << submit_ec.message();
   auto [write_ec, written] =
      co_await request.async_write(boost::asio::buffer("Hello, ", 7), token);
   EXPECT_FALSE(write_ec) << write_ec.message();
   auto [eof_ec, eof_written] =
      co_await request.async_write_eof(boost::asio::buffer("World", 5), token);
   EXPECT_FALSE(eof_ec) << eof_ec.message();

   auto [response_ec, response] = co_await request.async_get_response(token);
   EXPECT_FALSE(response_ec) << response_ec.message();
   EXPECT_EQ(response.status_code(), 200);
   auto body = co_await read_to_end(response, token);
   EXPECT_EQ(body, "Hello, World!");

   auto [shutdown_ec] = co_await session.async_shutdown(token);
   EXPECT_EQ(shutdown_ec, error_code{});
}

} // namespace

// =================================================================================================

class AsioTokens : public ClientAsync
{
protected:
   //
   // An operation that has completed cancels its cancel_after() timer. One that completed without
   // the timer hearing of it -- its handler dropped, say -- would leave the timer to keep the
   // context running until it fires, and the caller waiting for good.
   //
   void TearDown() override
   {
      const auto start = std::chrono::steady_clock::now();
      ClientAsync::TearDown();
      EXPECT_LT(std::chrono::steady_clock::now() - start, never / 2);
   }
};

INSTANTIATE_TEST_SUITE_P(AsioTokens, AsioTokens, ValuesIn(protocols()), NameGenerator);

// -------------------------------------------------------------------------------------------------

TEST_P(AsioTokens, WHEN_every_operation_takes_as_tuple_THEN_both_sides_see_all_of_the_body)
{
   requestHandler = [](server::Request request, server::Response response) {
      return echo(std::move(request), std::move(response), as_tuple);
   };
   clientSession = [this](Session session) -> Task<void> {
      co_await exchange(session, url, as_tuple);
   };
}

TEST_P(AsioTokens, WHEN_every_operation_takes_cancel_after_THEN_results_pass_through)
{
   requestHandler = [](server::Request request, server::Response response) {
      return echo(std::move(request), std::move(response), cancel_after(never, as_tuple));
   };
   clientSession = [this](Session session) -> Task<void> {
      co_await exchange(session, url, cancel_after(never, as_tuple));
   };
}

TEST_P(AsioTokens, WHEN_connect_and_get_take_cancel_after_THEN_results_pass_through)
{
   clientSession = [this](Session) -> Task<void> {
      auto [connect_ec, session] = co_await client->async_connect(cancel_after(never, as_tuple));
      EXPECT_FALSE(connect_ec) << connect_ec.message();
      auto [get_ec, message] =
         co_await session.async_get(url.set_path("missing"), {}, cancel_after(never, as_tuple));
      EXPECT_FALSE(get_ec) << get_ec.message();
      EXPECT_EQ(message.result_int(), 404);
   };
}

//
// The default token gives the result itself and throws the error. A request with a method takes
// no body, so it is complete as soon as it is submitted.
//
TEST_P(AsioTokens, WHEN_default_token_is_used_THEN_errors_are_thrown)
{
   requestHandler = [](server::Request, server::Response response) -> Task<void> {
      co_await response.async_submit(200, {});
      std::ignore = co_await response.async_write_eof(as_tuple);
   };
   clientSession = [this](Session session) -> Task<void> {
      auto request = co_await session.async_submit("GET", url, {});
      auto response = co_await request.async_get_response();
      EXPECT_EQ(response.status_code(), 200);
      co_await session.async_shutdown();

      error_code ec;
      try
      {
         co_await session.async_submit("GET", url, {});
      }
      catch (const boost::system::system_error& e)
      {
         ec = e.code();
      }
      EXPECT_TRUE(ec) << "a session that has been shut down takes no more requests";
   };
}

//
// ASIO's guarantee: a handler is not called from inside the function that starts the operation,
// not even when there is nothing left to wait for. A caller that holds a lock around the call, or
// starts the next operation from the handler, relies on it.
//
TEST_P(AsioTokens, WHEN_callback_is_given_THEN_it_is_not_called_before_the_operation_returns)
{
   clientSession = [this](Session session) -> Task<void> {
      std::optional<error_code> shutdown_result;
      session.async_shutdown([&](error_code ec) { shutdown_result = ec; });
      EXPECT_FALSE(shutdown_result) << "completed inside async_shutdown()";
      while (!shutdown_result)
         co_await yield();
      EXPECT_EQ(*shutdown_result, error_code{});

      //
      // A session that has been shut down fails a request right away: nothing to wait for.
      //
      std::optional<error_code> submit_result;
      session.async_submit(url, {}, [&](error_code ec, client::Request) { submit_result = ec; });
      EXPECT_FALSE(submit_result) << "completed inside async_submit()";
      while (!submit_result)
         co_await yield();
      EXPECT_TRUE(*submit_result);
   };
}

//
// Over HTTP/2 and HTTP/3, a response's submit completes inside the call on purpose, see
// NGHttp2Writer::async_submit(): a callback there cannot rely on ASIO's guarantee.
//
TEST_P(AsioTokens, WHEN_server_gives_a_callback_THEN_it_is_not_called_before_the_operation_returns)
{
   if (GetParam() != Protocol::h1)
      GTEST_SKIP() << "completes inside async_submit(), which costs less than posting it";

   requestHandler = [](server::Request, server::Response response) -> Task<void> {
      std::optional<error_code> result;
      response.async_submit(200, {}, [&](error_code ec) { result = ec; });
      EXPECT_FALSE(result) << "completed inside async_submit()";
      while (!result)
         co_await yield();
      EXPECT_EQ(*result, error_code{});
      std::ignore = co_await response.async_write_eof(as_tuple);
   };
   clientSession = [this](Session session) -> Task<void> {
      auto [ec, message] = co_await session.async_get(url, {}, as_tuple);
      EXPECT_FALSE(ec) << ec.message();
      EXPECT_EQ(message.result_int(), 200);
   };
}

// =================================================================================================

//
// A peer that takes what the client sends and never ends its side, so that a shutdown, after
// sending its own end, waits for the peer's in vain: for its "close_notify" over TLS, for its FIN
// in any case -- two seconds each, before the shutdown gives up on it. Over HTTP/2 it sends its
// SETTINGS first, which is all the client needs to see of a server.
//
class AsioTokensSilentPeer : public TestWithParam<std::tuple<Protocol, bool>>
{
protected:
   void SetUp() override { setup_logging(); }

   Protocol protocol() const { return std::get<0>(GetParam()); }
   bool tls() const { return std::get<1>(GetParam()); }

   Task<void> serve()
   {
      auto socket = co_await acceptor.async_accept(use_awaitable);
      if (tls())
      {
         auto& stream = tls_peer.emplace(std::move(socket), tls_context);
         co_await stream.async_handshake(boost::asio::ssl::stream_base::server, use_awaitable);
         co_await read_on(stream);
      }
      else
         co_await read_on(peer.emplace(std::move(socket)));
   }

   /// Reads until the client has ended its side, and leaves the connection as it is.
   template <typename Stream>
   Task<void> read_on(Stream& stream)
   {
      if (protocol() == Protocol::h2)
         co_await boost::asio::async_write(stream, boost::asio::buffer("\0\0\0\x04\0\0\0\0\0"sv),
                                           use_awaitable);

      std::array<char, 4096> buffer;
      for (;;)
      {
         auto [ec, n] = co_await stream.async_read_some(boost::asio::buffer(buffer), as_tuple);
         if (ec)
            break;
      }
   }

   boost::urls::url url() const
   {
      boost::urls::url url(tls() ? "https://127.0.0.2/" : "http://127.0.0.2/");
      url.set_port_number(acceptor.local_endpoint().port());
      return url;
   }

   IoContext context;
   tcp::acceptor acceptor{context, {boost::asio::ip::make_address("127.0.0.2"), 0}};
   boost::asio::ssl::context tls_context = make_server_tls_context(
      server::Config{}.tls_certificate_chain, server::Config{}.tls_private_key);
   std::optional<tcp::socket> peer;
   std::optional<boost::asio::ssl::stream<tcp::socket>> tls_peer;
};

INSTANTIATE_TEST_SUITE_P(AsioTokensSilentPeer, AsioTokensSilentPeer,
                         Combine(Values(Protocol::h1, Protocol::h2), Bool()), [](const auto& info) {
                            return to_string(std::get<0>(info.param)) +
                                   (std::get<1>(info.param) ? "_TLS" : "_Cleartext");
                         });

//
// The caller's cancellation reaches the wait, and the session takes no more requests afterwards.
// What the shutdown reports then, and whether the connection is closed by the time it does, is not
// settled yet: it differs between the protocols, and between cleartext and TLS.
//
TEST_P(AsioTokensSilentPeer, WHEN_shutdown_takes_cancel_after_THEN_it_ends_the_wait)
{
   co_spawn(context, serve(), detached);
   co_spawn(
      context,
      [this]() -> Task<void> {
         client::Client client(context.get_executor(),
                               client::Config{.url = url(),
                                              .protocol = protocol(),
                                              .tls_ca_file = "pki/out/root.pem"});
         auto [connect_ec, session] = co_await client.async_connect(as_tuple);
         EXPECT_FALSE(connect_ec) << connect_ec.message();

         const auto start = std::chrono::steady_clock::now();
         std::ignore = co_await session.async_shutdown(cancel_after(100ms, as_tuple));
         EXPECT_LT(std::chrono::steady_clock::now() - start, 1s)
            << "cancel_after did not reach the wait";

         auto [submit_ec, request] = co_await session.async_submit(url(), {}, as_tuple);
         EXPECT_TRUE(submit_ec) << "a session that has been shut down takes no more requests";

         acceptor.close();
      },
      rethrow);
   context.run();
}

// =================================================================================================

//
// A handle without an implementation -- default-constructed, moved from or reset -- fails what is
// started on it with bad_descriptor. It has no executor, though: a plain callback is called from
// inside the call, and a token that needs an executor from the operation, as cancel_after() does
// for its timer, throws bad_executor before anything has started.
//
class AsioTokensEmptyHandle : public Test
{
protected:
   void SetUp() override { setup_logging(); }

   IoContext context;
   client::Request request;
   client::Response response;
   quic::Connection connection;
   std::array<char, 16> buffer;
};

TEST_F(AsioTokensEmptyHandle, WHEN_awaited_THEN_operations_fail_with_bad_descriptor)
{
   co_spawn(
      context,
      [this]() -> Task<void> {
         auto [read_ec, n] =
            co_await response.async_read_some(boost::asio::buffer(buffer), as_tuple);
         EXPECT_EQ(read_ec, errors::bad_descriptor);
         auto [write_ec, written] =
            co_await request.async_write(boost::asio::buffer(buffer), as_tuple);
         EXPECT_EQ(write_ec, errors::bad_descriptor);
         auto [eof_ec, eof_written] = co_await request.async_write_eof(as_tuple);
         EXPECT_EQ(eof_ec, errors::bad_descriptor);
         auto [response_ec, r] = co_await request.async_get_response(as_tuple);
         EXPECT_EQ(response_ec, errors::bad_descriptor);
         auto [accept_ec, stream] = co_await connection.async_accept_stream(as_tuple);
         EXPECT_EQ(accept_ec, errors::bad_descriptor);
      },
      rethrow);
   context.run();
}

// =================================================================================================

#endif
