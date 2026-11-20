#include "test_fixtures.hpp"

#include "anyhttp/h1/io.hpp"
#include "anyhttp/net.hpp"

#include <boost/http/request.hpp>
#include <boost/http/response_parser.hpp>
#include <boost/http/serializer.hpp>
#include <boost/scope/scope_exit.hpp>

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <map>
#include <ranges>
#include <span>
#include <string>
#include <vector>

using namespace testing;

namespace h1 = anyhttp::h1;

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
   using Http11Response = client::Message;

   /// A request with a body, which a header alone has no place for.
   struct Request : boost::http::request
   {
      using boost::http::request::request;
      std::string body;
   };

   /// base64url without padding (RFC 4648, section 5), as 'HTTP2-Settings' has it.
   static std::string base64url(std::span<const uint8_t> data)
   {
      static constexpr std::string_view alphabet =
         "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
      std::string result;
      for (size_t i = 0; i < data.size(); i += 3)
      {
         const size_t n = std::min<size_t>(3, data.size() - i);
         uint32_t bits = 0;
         for (size_t j = 0; j < 3; ++j)
            bits = (bits << 8) | (j < n ? data[i + j] : 0);
         for (size_t j = 0; j <= n; ++j)
            result.push_back(alphabet[(bits >> (18 - 6 * j)) & 0x3f]);
      }
      return result;
   }

   static boost::http::response_parser make_parser()
   {
      boost::http::response_parser parser(h1::make_parser_config(false, default_max_header_size));
      parser.reset();
      parser.start();
      return parser;
   }

   static nghttp2_nv nv(std::string_view name, std::string_view value)
   {
      return {const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name.data())),
              const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value.data())), name.size(),
              value.size(), NGHTTP2_NV_FLAG_NONE};
   }

   /// Upgrades a request for the first target, with \p method, and sends GETs for the others as
   /// HTTP/2 streams. \p fields go along with the upgrade request.
   Task<Responses> upgrade(std::vector<std::string> targets, Fields fields = {},
                           boost::http::method method = boost::http::method::get)
   {
      namespace http = boost::http;

      auto socket = io::make_socket(context.get_executor());
      check(co_await io::connect(socket, {server->local_endpoint()}));
      const auto authority = std::format("127.0.0.2:{}", server->local_endpoint().port());

      Responses responses;
      auto callbacks = std::invoke([] {
         nghttp2_session_callbacks* cbs;
         nghttp2_session_callbacks_new(&cbs);
         nghttp2_session_callbacks_set_on_header_callback(
            cbs,
            [](nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t namelen,
               const uint8_t* value, size_t valuelen, uint8_t, void* user_data) -> int {
               auto& responses = *static_cast<Responses*>(user_data);
               if (std::string_view(reinterpret_cast<const char*>(name), namelen) == ":status")
                  responses[frame->hd.stream_id].status =
                     std::stoul(std::string(reinterpret_cast<const char*>(value), valuelen));
               return 0;
            });
         nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
            cbs,
            [](nghttp2_session*, uint8_t, int32_t stream_id, const uint8_t* data, size_t len,
               void* user_data) -> int {
               auto& responses = *static_cast<Responses*>(user_data);
               responses[stream_id].body.append(reinterpret_cast<const char*>(data), len);
               return 0;
            });
         nghttp2_session_callbacks_set_on_stream_close_callback(
            cbs, [](nghttp2_session*, int32_t stream_id, uint32_t, void* user_data) -> int {
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
      auto iv =
         std::to_array<nghttp2_settings_entry>({{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}});
      std::array<uint8_t, 16> settings;
      auto len =
         nghttp2_pack_settings_payload2(settings.data(), settings.size(), iv.data(), iv.size());
      EXPECT_GT(len, 0);

      http::request request(method, targets.front());
      for (const auto& field : fields)
         request.append(field.name, field.value);
      request.set(http::field::host, authority);
      request.set(http::field::connection, "Upgrade, HTTP2-Settings");
      request.set(http::field::upgrade, "h2c");
      request.set(http::field::http2_settings, base64url({settings.data(), size_t(len)}));
      http::serializer serializer(h1::make_serializer_config());
      check(co_await h1::write_message(socket, serializer, request));

      Buffer buffer;
      auto parser = make_parser();
      check(co_await h1::read_header(socket, buffer, parser));
      EXPECT_EQ(parser.get().status(), http::status::switching_protocols);
      if (parser.get().status() != http::status::switching_protocols)
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

      auto recv = [&](boost::asio::const_buffer data) {
         auto n = nghttp2_session_mem_recv2(session, static_cast<const uint8_t*>(data.data()),
                                            data.size());
         EXPECT_EQ(n, data.size()) << nghttp2_strerror(n);
      };

      auto done = [&] {
         return std::ranges::count_if(responses, [](auto& item) { return item.second.closed; }) ==
                targets.size();
      };

      std::string out; // nghttp2 starts with the client magic by itself
      auto send = [&]() -> Task<void> {
         const uint8_t* data;
         while (auto n = nghttp2_session_mem_send2(session, &data))
         {
            EXPECT_GT(n, 0) << nghttp2_strerror(n);
            if (n < 0)
               break;
            out.append(reinterpret_cast<const char*>(data), n);
         }
         if (!out.empty())
            check(co_await io::write(socket, boost::asio::buffer(out)));
         out.clear();
      };

      // what came along with the 101 response
      const auto leftover = parser.release_buffered_data();
      recv(boost::asio::buffer(leftover.data(), leftover.size()));
      std::array<uint8_t, 16384> data;
      for (co_await send(); !done(); co_await send())
      {
         auto [ec, n] = co_await io::read_some(socket, boost::asio::buffer(data));
         EXPECT_FALSE(ec) << ec.message();
         if (ec)
            break;
         recv(boost::asio::buffer(data, n));
      }

      nghttp2_session_terminate_session(session, NGHTTP2_NO_ERROR);
      co_await send();
      io::shutdown(socket, io::Shutdown::send); // the server may have closed the connection already
      co_return responses;
   }

   /// Sends a single HTTP/1.1 request and reads the response.
   Task<Http11Response> http11(Request request)
   {
      namespace http = boost::http;

      auto socket = io::make_socket(context.get_executor());
      check(co_await io::connect(socket, {server->local_endpoint()}));

      request.set(http::field::host, std::format("127.0.0.2:{}", server->local_endpoint().port()));
      if (!request.body.empty() || request.method() != http::method::get)
         request.set_content_length(request.body.size());
      http::serializer serializer(h1::make_serializer_config());
      check(co_await h1::write_message(socket, serializer, request, request.body));

      Buffer buffer;
      auto parser = make_parser();
      auto body = check(co_await h1::read_message(socket, buffer, parser));
      Http11Response response(parser.get());
      response.body() = std::move(body);
      io::shutdown(socket, io::Shutdown::send); // the server may have closed the connection already
      co_return response;
   }

   /// Runs \p task to completion, stops the server and returns the result.
   template <typename T>
   T run(Task<T> task)
   {
      T result;
      auto store = [](Task<T> task, T& result) -> Task<void> { result = co_await std::move(task); };
      launch(context.get_executor(), store(std::move(task), result),
             [this](const std::exception_ptr& ep) {
                if (ep)
                   ADD_FAILURE() << what(ep);
                server.reset();
             });
      Server::run();
      return result;
   }

   static Request upgrade_request(boost::http::method method, std::string target)
   {
      namespace http = boost::http;
      Request request(method, target);
      request.set(http::field::connection, "Upgrade, HTTP2-Settings");
      request.set(http::field::upgrade, "h2c");
      request.set(http::field::http2_settings, "AAMAAABkAAQAAQAAAAIAAAAA"); // as sent by curl
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
   EXPECT_THAT(responses[1].body, HasSubstr("method: GET")); // carried over by the upgrade
   EXPECT_THAT(responses[1].body, HasSubstr("path: /dump"));
   EXPECT_THAT(responses[1].body, HasSubstr("query: first"));
}

TEST_F(H2CUpgrade, WHEN_upgraded_THEN_request_headers_are_passed_on_to_stream_1)
{
   Fields fields;
   fields.set("x-custom", "value");
   fields.set(boost::http::field::keep_alive, "timeout=5");
   auto responses = run(upgrade({"/dump?first"}, std::move(fields)));

   ASSERT_TRUE(responses.contains(1));
   EXPECT_EQ(responses[1].status, 200);
   const auto& body = responses[1].body;
   EXPECT_THAT(body, HasSubstr("\n  x-custom: value\n"));
   EXPECT_THAT(body, HasSubstr("\n  Host: 127.0.0.2:"));

   // connection-specific fields do not exist in HTTP/2 (RFC 9113, section 8.2.2)
   EXPECT_THAT(body, Not(HasSubstr("Connection:")));
   EXPECT_THAT(body, Not(HasSubstr("Upgrade:")));
   EXPECT_THAT(body, Not(HasSubstr("HTTP2-Settings:")));
   EXPECT_THAT(body, Not(HasSubstr("Keep-Alive:")));
}

TEST_F(H2CUpgrade, WHEN_upgraded_THEN_connection_takes_more_streams)
{
   auto responses = run(upgrade({"/dump?first", "/dump?second", "/unknown"}));

   ASSERT_EQ(responses.size(), 3);
   EXPECT_EQ(responses[1].status, 200);
   EXPECT_THAT(responses[1].body, HasSubstr("query: first"));
   EXPECT_EQ(responses[3].status, 200);
   EXPECT_THAT(responses[3].body, HasSubstr("query: second"));
   EXPECT_EQ(responses[5].status, 404);
}

//
// Upgrading with an OPTIONS request is what RFC 7540, section 3.2 suggests when the first real
// request must not hold up the connection: it has no body to be sent in HTTP/1.1 first, so the
// requests that matter all start as HTTP/2 streams, concurrently.
//
TEST_F(H2CUpgrade, WHEN_options_is_upgraded_THEN_method_is_carried_over_to_stream_1)
{
   auto responses = run(upgrade({"/dump?options"}, {}, boost::http::method::options));

   ASSERT_EQ(responses.size(), 1);
   ASSERT_TRUE(responses.contains(1));
   EXPECT_EQ(responses[1].status, 200);
   EXPECT_TRUE(responses[1].closed);
   EXPECT_THAT(responses[1].body, HasSubstr("method: OPTIONS"));
   EXPECT_THAT(responses[1].body, HasSubstr("path: /dump"));
   EXPECT_THAT(responses[1].body, HasSubstr("query: options"));
}

TEST_F(H2CUpgrade, WHEN_options_asterisk_is_upgraded_THEN_connection_takes_more_streams)
{
   auto responses = run(upgrade({"*", "/dump?second"}, {}, boost::http::method::options));

   ASSERT_EQ(responses.size(), 2);
   EXPECT_EQ(responses[1].status, 404); // the test server has no handler for an asterisk target
   EXPECT_TRUE(responses[1].closed);
   EXPECT_EQ(responses[3].status, 200);
   EXPECT_THAT(responses[3].body, HasSubstr("query: second"));
}

TEST_F(H2CUpgrade, WHEN_request_has_body_THEN_is_served_as_http11)
{
   auto request = upgrade_request(boost::http::method::post, "/echo");
   request.body = "Hello, World!";
   auto response = run(http11(std::move(request)));

   EXPECT_EQ(response.status_int(), 200);
   EXPECT_EQ(response.body(), "Hello, World!");
}

TEST_F(H2CUpgrade, WHEN_http2_settings_are_missing_THEN_is_served_as_http11)
{
   auto request = upgrade_request(boost::http::method::get, "/dump?no-settings");
   request.erase(boost::http::field::http2_settings);
   auto response = run(http11(std::move(request)));

   EXPECT_EQ(response.status_int(), 200);
   EXPECT_THAT(response.body(), HasSubstr("query: no-settings"));
}

TEST_F(H2CUpgrade, WHEN_http2_settings_are_invalid_THEN_is_served_as_http11)
{
   auto request = upgrade_request(boost::http::method::get, "/dump?invalid");
   request.set(boost::http::field::http2_settings, "AAMAAABkAA"); // 7 bytes, not a multiple of 6
   auto response = run(http11(std::move(request)));

   EXPECT_EQ(response.status_int(), 200);
   EXPECT_THAT(response.body(), HasSubstr("query: invalid"));
}

// =================================================================================================
