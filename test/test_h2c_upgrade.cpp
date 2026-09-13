#include "test_fixtures.hpp"

#include <boost/beast/core/detail/base64.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <map>
#include <ranges>
#include <span>
#include <string>
#include <vector>

// =================================================================================================

//
// Upgrade from HTTP/1.1 to cleartext HTTP/2 (RFC 7540, section 3.2). The HTTP/2 side of the client
// is a bare nghttp2 session driven by hand, so that the test is in control of the handshake.
//
class H2CUpgrade : public Server
{
protected:
   struct Response
   {
      unsigned status = 0;
      std::string body;
      bool closed = false;
   };

   using Responses = std::map<int32_t, Response>;
   using Request = boost::beast::http::request<boost::beast::http::string_body>;
   using Http11Response = boost::beast::http::response<boost::beast::http::string_body>;

   static std::string base64url(std::span<const uint8_t> data)
   {
      namespace base64 = boost::beast::detail::base64;
      std::string result(base64::encoded_size(data.size()), '\0');
      result.resize(base64::encode(result.data(), data.data(), data.size()));
      std::ranges::replace(result, '+', '-');
      std::ranges::replace(result, '/', '_');
      while (result.ends_with('='))
         result.pop_back();
      return result;
   }

   static nghttp2_nv nv(std::string_view name, std::string_view value)
   {
      return {const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name.data())),
              const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value.data())), name.size(),
              value.size(), NGHTTP2_NV_FLAG_NONE};
   }

   /// Upgrades a GET for the first target and sends GETs for the others as HTTP/2 streams.
   awaitable<Responses> upgrade(std::vector<std::string> targets)
   {
      namespace http = boost::beast::http;

      tcp::socket socket(co_await this_coro::executor);
      co_await socket.async_connect(server->local_endpoint());
      const auto authority = std::format("127.0.0.2:{}", server->local_endpoint().port());

      Responses responses;
      auto callbacks = std::invoke([]
      {
         nghttp2_session_callbacks* cbs;
         nghttp2_session_callbacks_new(&cbs);
         nghttp2_session_callbacks_set_on_header_callback(
            cbs,
            [](nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t namelen,
               const uint8_t* value, size_t valuelen, uint8_t, void* user_data) -> int
         {
            auto& responses = *static_cast<Responses*>(user_data);
            if (std::string_view(reinterpret_cast<const char*>(name), namelen) == ":status")
               responses[frame->hd.stream_id].status =
                  std::stoul(std::string(reinterpret_cast<const char*>(value), valuelen));
            return 0;
         });
         nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
            cbs,
            [](nghttp2_session*, uint8_t, int32_t stream_id, const uint8_t* data, size_t len,
               void* user_data) -> int
         {
            auto& responses = *static_cast<Responses*>(user_data);
            responses[stream_id].body.append(reinterpret_cast<const char*>(data), len);
            return 0;
         });
         nghttp2_session_callbacks_set_on_stream_close_callback(
            cbs, [](nghttp2_session*, int32_t stream_id, uint32_t, void* user_data) -> int
         {
            static_cast<Responses*>(user_data)->operator[](stream_id).closed = true;
            return 0;
         });
         return std::unique_ptr<nghttp2_session_callbacks, void (*)(nghttp2_session_callbacks*)>(
            cbs, nghttp2_session_callbacks_del);
      });

      nghttp2_session* session;
      nghttp2_session_client_new(&session, callbacks.get(), &responses);
      boost::scope::scope_exit deleter([&] { nghttp2_session_del(session); });

      //
      // HTTP/1.1 request asking for the upgrade
      //
      std::array<nghttp2_settings_entry, 1> iv{{{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}}};
      std::array<uint8_t, 16> settings;
      auto len =
         nghttp2_pack_settings_payload2(settings.data(), settings.size(), iv.data(), iv.size());
      EXPECT_GT(len, 0);

      http::request<http::empty_body> request{http::verb::get, targets.front(), 11};
      request.set(http::field::host, authority);
      request.set(http::field::connection, "Upgrade, HTTP2-Settings");
      request.set(http::field::upgrade, "h2c");
      request.set("HTTP2-Settings", base64url({settings.data(), size_t(len)}));
      co_await http::async_write(socket, request);

      boost::beast::flat_buffer buffer;
      http::response_parser<http::empty_body> parser;
      co_await http::async_read_header(socket, buffer, parser);
      EXPECT_EQ(parser.get().result(), http::status::switching_protocols);
      if (parser.get().result() != http::status::switching_protocols)
         co_return responses;

      //
      // From here on, it's HTTP/2: the upgraded request continues as stream 1.
      //
      auto result = nghttp2_session_upgrade2(session, settings.data(), len, 0, nullptr);
      EXPECT_EQ(result, 0) << nghttp2_strerror(result);
      if (result)
         co_return responses;

      nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv.data(), iv.size());
      for (auto& target : targets | rv::drop(1))
      {
         std::array nva{nv(":method", "GET"), nv(":scheme", "http"), nv(":authority", authority),
                        nv(":path", target)};
         auto id =
            nghttp2_submit_request2(session, nullptr, nva.data(), nva.size(), nullptr, nullptr);
         EXPECT_GT(id, 0) << nghttp2_strerror(id);
      }

      auto recv = [&](const_buffer data)
      {
         auto n = nghttp2_session_mem_recv2(session, static_cast<const uint8_t*>(data.data()),
                                            data.size());
         EXPECT_EQ(n, data.size()) << nghttp2_strerror(n);
      };

      auto done = [&]
      {
         return std::ranges::count_if(responses, [](auto& item) { return item.second.closed; }) ==
                targets.size();
      };

      std::string out; // nghttp2 starts with the client magic by itself
      auto send = [&]() -> awaitable<void>
      {
         const uint8_t* data;
         while (auto n = nghttp2_session_mem_send2(session, &data))
         {
            EXPECT_GT(n, 0) << nghttp2_strerror(n);
            if (n < 0)
               break;
            out.append(reinterpret_cast<const char*>(data), n);
         }
         if (!out.empty())
            co_await asio::async_write(socket, asio::buffer(out));
         out.clear();
      };

      recv(buffer.data()); // what came along with the 101 response
      std::array<uint8_t, 16384> data;
      for (co_await send(); !done(); co_await send())
      {
         auto [ec, n] = co_await socket.async_read_some(asio::buffer(data), as_tuple);
         EXPECT_FALSE(ec) << ec.message();
         if (ec)
            break;
         recv(asio::buffer(data, n));
      }

      nghttp2_session_terminate_session(session, NGHTTP2_NO_ERROR);
      co_await send();
      boost::system::error_code ignored; // the server may have closed the connection already
      socket.shutdown(tcp::socket::shutdown_send, ignored);
      co_return responses;
   }

   /// Sends a single HTTP/1.1 request and reads the response.
   awaitable<Http11Response> http11(Request request)
   {
      namespace http = boost::beast::http;

      tcp::socket socket(co_await this_coro::executor);
      co_await socket.async_connect(server->local_endpoint());

      request.set(http::field::host, std::format("127.0.0.2:{}", server->local_endpoint().port()));
      request.prepare_payload();
      co_await http::async_write(socket, request);

      boost::beast::flat_buffer buffer;
      Http11Response response;
      co_await http::async_read(socket, buffer, response);
      boost::system::error_code ignored; // the server may have closed the connection already
      socket.shutdown(tcp::socket::shutdown_send, ignored);
      co_return response;
   }

   /// Runs \p task to completion, stops the server and returns the result.
   template <typename T>
   T run(awaitable<T> task)
   {
      T result;
      co_spawn(context, std::move(task), [&](const std::exception_ptr& ep, T value)
      {
         if (ep)
            ADD_FAILURE() << what(ep);
         result = std::move(value);
         server.reset();
      });
      Server::run();
      return result;
   }

   static Request upgrade_request(boost::beast::http::verb method, std::string target)
   {
      namespace http = boost::beast::http;
      Request request{method, target, 11};
      request.set(http::field::connection, "Upgrade, HTTP2-Settings");
      request.set(http::field::upgrade, "h2c");
      request.set("HTTP2-Settings", "AAMAAABkAAQAAQAAAAIAAAAA"); // as sent by curl
      return request;
   }
};

// -------------------------------------------------------------------------------------------------

TEST_F(H2CUpgrade, WHEN_upgrade_is_requested_THEN_request_continues_as_stream_1)
{
   auto responses = run(upgrade({"/dump?first"}));

   ASSERT_EQ(responses.size(), 1);
   ASSERT_TRUE(responses.contains(1));
   EXPECT_EQ(responses[1].status, 200);
   EXPECT_TRUE(responses[1].closed);
   EXPECT_THAT(responses[1].body, testing::HasSubstr("path: /dump"));
   EXPECT_THAT(responses[1].body, testing::HasSubstr("query: first"));
}

TEST_F(H2CUpgrade, WHEN_upgraded_THEN_connection_takes_more_streams)
{
   auto responses = run(upgrade({"/dump?first", "/dump?second", "/unknown"}));

   ASSERT_EQ(responses.size(), 3);
   EXPECT_EQ(responses[1].status, 200);
   EXPECT_THAT(responses[1].body, testing::HasSubstr("query: first"));
   EXPECT_EQ(responses[3].status, 200);
   EXPECT_THAT(responses[3].body, testing::HasSubstr("query: second"));
   EXPECT_EQ(responses[5].status, 404);
}

TEST_F(H2CUpgrade, WHEN_request_has_body_THEN_is_served_as_http11)
{
   auto request = upgrade_request(boost::beast::http::verb::post, "/echo");
   request.body() = "Hello, World!";
   auto response = run(http11(std::move(request)));

   EXPECT_EQ(response.result_int(), 200);
   EXPECT_EQ(response.body(), "Hello, World!");
}

TEST_F(H2CUpgrade, WHEN_http2_settings_are_missing_THEN_is_served_as_http11)
{
   auto request = upgrade_request(boost::beast::http::verb::get, "/dump?no-settings");
   request.erase("HTTP2-Settings");
   auto response = run(http11(std::move(request)));

   EXPECT_EQ(response.result_int(), 200);
   EXPECT_THAT(response.body(), testing::HasSubstr("query: no-settings"));
}

TEST_F(H2CUpgrade, WHEN_http2_settings_are_invalid_THEN_is_served_as_http11)
{
   auto request = upgrade_request(boost::beast::http::verb::get, "/dump?invalid");
   request.set("HTTP2-Settings", "AAMAAABkAA"); // 7 bytes, not a multiple of 6
   auto response = run(http11(std::move(request)));

   EXPECT_EQ(response.result_int(), 200);
   EXPECT_THAT(response.body(), testing::HasSubstr("query: invalid"));
}

// =================================================================================================
