#include "test_fixtures.hpp"

#include "anyhttp/alt_svc.hpp"

#include <boost/beast/http/field.hpp>

#include <nghttp2/nghttp2.h>

#include <array>
#include <string>

using namespace testing;

namespace http = boost::beast::http;

// =================================================================================================
// Parsing the field value, which is the same for the header field and the HTTP/2 ALTSVC frame.
// =================================================================================================

TEST(AltSvc, WHEN_a_single_alternative_is_given_THEN_it_is_parsed)
{
   auto alt_svc = parse_alt_svc(R"(h3=":443")");
   ASSERT_EQ(alt_svc.services.size(), 1u);

   const auto& service = alt_svc.services.front();
   EXPECT_EQ(service.protocol, "h3");
   EXPECT_EQ(service.host, "");
   EXPECT_EQ(service.port, "443");
   EXPECT_EQ(service.max_age, 24h) << "the default of RFC 7838, section 3.1";
   EXPECT_FALSE(service.persist);
}

TEST(AltSvc, WHEN_parameters_are_given_THEN_they_are_parsed)
{
   auto alt_svc = parse_alt_svc(R"(h3="alt.example.com:8443"; ma=3600; persist=1)");
   ASSERT_EQ(alt_svc.services.size(), 1u);

   const auto& service = alt_svc.services.front();
   EXPECT_EQ(service.protocol, "h3");
   EXPECT_EQ(service.host, "alt.example.com");
   EXPECT_EQ(service.port, "8443");
   EXPECT_EQ(service.max_age, 1h);
   EXPECT_TRUE(service.persist);
}

TEST(AltSvc, WHEN_alternatives_are_listed_THEN_the_order_is_kept)
{
   auto alt_svc = parse_alt_svc(R"(h2=":443", h3=":443"; ma=60 , h3-29=":444")");
   ASSERT_EQ(alt_svc.services.size(), 3u);
   EXPECT_EQ(alt_svc.services[0].protocol, "h2");
   EXPECT_EQ(alt_svc.services[1].protocol, "h3");
   EXPECT_EQ(alt_svc.services[2].protocol, "h3-29");

   //
   // find() answers with the first alternative for a protocol, which is the one the server
   // prefers -- "h3-29" is a protocol of its own and not an "h3".
   //
   const auto* h3 = alt_svc.find("h3");
   ASSERT_NE(h3, nullptr);
   EXPECT_EQ(h3->max_age, 60s);
   EXPECT_EQ(alt_svc.find("h4"), nullptr);
}

TEST(AltSvc, WHEN_the_host_is_an_IPv6_literal_THEN_the_brackets_are_taken_off)
{
   auto alt_svc = parse_alt_svc(R"(h3="[::1]:443")");
   ASSERT_EQ(alt_svc.services.size(), 1u);
   EXPECT_EQ(alt_svc.services.front().host, "::1") << "a resolver wants it without them";
   EXPECT_EQ(alt_svc.services.front().port, "443");
}

TEST(AltSvc, WHEN_the_protocol_is_percent_encoded_THEN_it_is_decoded)
{
   auto alt_svc = parse_alt_svc(R"(%68%33=":443")");
   ASSERT_EQ(alt_svc.services.size(), 1u);
   EXPECT_EQ(alt_svc.services.front().protocol, "h3");
}

TEST(AltSvc, WHEN_the_value_is_clear_THEN_it_says_so)
{
   EXPECT_TRUE(parse_alt_svc("clear").clear);
   EXPECT_TRUE(parse_alt_svc("  clear  ").clear);
   EXPECT_TRUE(parse_alt_svc("clear").services.empty());

   // "clear" is the whole field value and nothing else, so as a list element it is just garbage
   auto alt_svc = parse_alt_svc(R"(clear, h3=":443")");
   EXPECT_FALSE(alt_svc.clear);
   EXPECT_EQ(alt_svc.services.size(), 1u);
}

TEST(AltSvc, WHEN_an_alternative_is_malformed_THEN_the_others_are_still_read)
{
   //
   // A missing alt-authority, one without a port, an unterminated quoted string, a parameter
   // without a value: every one of those takes its own alternative down and nothing else.
   //
   EXPECT_THAT(parse_alt_svc(R"(h3, h2=":443")").services, SizeIs(1));
   EXPECT_THAT(parse_alt_svc(R"(h3="example.com", h2=":443")").services, SizeIs(1));
   EXPECT_THAT(parse_alt_svc(R"(h3=":443"; ma, h2=":443")").services, SizeIs(1));
   EXPECT_THAT(parse_alt_svc(R"(h3="unterminated, h2=":443")").services, IsEmpty());
   EXPECT_THAT(parse_alt_svc(R"(h3=":443"; ma=x, h2=":443")").services, SizeIs(2))
      << "an unreadable parameter value leaves the alternative itself alone";

   EXPECT_THAT(parse_alt_svc("").services, IsEmpty());
   EXPECT_THAT(parse_alt_svc(",,,").services, IsEmpty());
   EXPECT_THAT(parse_alt_svc("garbage").services, IsEmpty());
}

TEST(AltSvc, WHEN_the_alternative_expires_immediately_THEN_max_age_is_zero)
{
   auto alt_svc = parse_alt_svc(R"(h3=":443"; ma=0)");
   ASSERT_EQ(alt_svc.services.size(), 1u);
   EXPECT_EQ(alt_svc.services.front().max_age, 0s);
}

// =================================================================================================
// Taking up the alternative: the server advertises its HTTP/3 endpoint over HTTP/1.1 and HTTP/2,
// and the next connection of a client that follows it is made over QUIC.
// =================================================================================================

//
// What a response says about the protocol that carried it: only the HTTP/3 server sends this
// "server" field, and only HTTP/1.1 and HTTP/2 advertise an alternative in the first place.
//
constexpr auto http3_server = "anyhttp-quic/0.1"sv;

static bool served_over_http3(const client::Message& message)
{
   return message[http::field::server] == http3_server;
}

class AltSvcUpgrade : public ClientAsync
{
protected:
   void configure_client(client::Config& config) override { config.follow_alt_svc = true; }

   /// The fixture's request handler knows this one, and it needs neither a body nor a handler.
   boost::urls::url echo() const
   {
      auto target = url;
      target.set_path("/echo");
      return target;
   }
};

//
// Only HTTP/1.1 and HTTP/2 have anywhere to go: a client that already speaks HTTP/3 is there.
//
INSTANTIATE_TEST_SUITE_P(AltSvcUpgrade, AltSvcUpgrade,
                         Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2), NameGenerator);

TEST_P(AltSvcUpgrade, WHEN_the_server_advertises_h3_THEN_the_next_connection_uses_it)
{
   clientSession = [this](Session session) -> awaitable<void> {
      auto first = co_await session.async_get(echo());
      EXPECT_EQ(first.result_int(), 200);
      EXPECT_FALSE(served_over_http3(first));
      EXPECT_THAT(std::string(first[http::field::alt_svc]), HasSubstr("h3="));

      auto second = co_await (co_await client->async_connect()).async_get(echo());
      EXPECT_EQ(second.result_int(), 200);
      EXPECT_TRUE(served_over_http3(second)) << "the second connection is not HTTP/3";
      EXPECT_EQ(second[http::field::alt_svc], "")
         << "HTTP/3 has no alternative to advertise, it is the alternative";
   };
}

//
// The session that learns about the alternative keeps speaking what it speaks: there is no
// in-band upgrade to HTTP/3, and a connection in the middle of a request can not be moved.
//
TEST_P(AltSvcUpgrade, WHEN_the_alternative_is_learned_THEN_the_session_that_learned_it_stays)
{
   clientSession = [this](Session session) -> awaitable<void> {
      EXPECT_FALSE(served_over_http3(co_await session.async_get(echo())));
      EXPECT_FALSE(served_over_http3(co_await session.async_get(echo())));
   };
}

TEST_P(AltSvcUpgrade, WHEN_the_server_clears_the_alternative_THEN_it_is_not_used)
{
   requestHandler = [](server::Request request, server::Response response) -> awaitable<void> {
      co_await drain(request);
      co_await response.async_submit(200, fields({{"Alt-Svc", "clear"}, {"Content-Length", 0}}));
      co_await response.async_write_eof();
   };

   clientSession = [this](Session session) -> awaitable<void> {
      EXPECT_THAT(std::string((co_await session.async_get(echo()))[http::field::alt_svc]),
                  HasSubstr("h3="));

      auto cleared = co_await session.async_get(url); // "/custom", the handler above
      EXPECT_EQ(cleared[http::field::alt_svc], "clear") << "the handler's field beats ours";

      auto second = co_await (co_await client->async_connect()).async_get(echo());
      EXPECT_FALSE(served_over_http3(second)) << "the alternative should have been forgotten";
   };
}

// -------------------------------------------------------------------------------------------------

//
// Without Config::follow_alt_svc, the advertisement is still there to be seen, but nothing is
// done with it: a client asked for a protocol gets that protocol.
//
class AltSvcIgnored : public ClientAsync
{
};

INSTANTIATE_TEST_SUITE_P(AltSvcIgnored, AltSvcIgnored,
                         Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2), NameGenerator);

TEST_P(AltSvcIgnored, WHEN_the_client_does_not_follow_alt_svc_THEN_it_keeps_its_protocol)
{
   clientSession = [this](Session session) -> awaitable<void> {
      auto target = url;
      target.set_path("/echo");

      auto first = co_await session.async_get(target);
      EXPECT_THAT(std::string(first[http::field::alt_svc]), HasSubstr("h3="));

      auto second = co_await (co_await client->async_connect()).async_get(target);
      EXPECT_FALSE(served_over_http3(second));
   };
}

// -------------------------------------------------------------------------------------------------

//
// A server that advertises nothing, see server::Config::alt_svc_max_age.
//
class AltSvcDisabled : public ClientAsync
{
protected:
   void configure_server(server::Config& config) override { config.alt_svc_max_age = 0s; }
   void configure_client(client::Config& config) override { config.follow_alt_svc = true; }
};

INSTANTIATE_TEST_SUITE_P(AltSvcDisabled, AltSvcDisabled,
                         Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2), NameGenerator);

TEST_P(AltSvcDisabled, WHEN_the_server_advertises_nothing_THEN_the_client_stays_where_it_is)
{
   clientSession = [this](Session session) -> awaitable<void> {
      auto target = url;
      target.set_path("/echo");

      auto first = co_await session.async_get(target);
      EXPECT_EQ(first[http::field::alt_svc], "");

      auto second = co_await (co_await client->async_connect()).async_get(target);
      EXPECT_FALSE(served_over_http3(second));
   };
}

// =================================================================================================
// The HTTP/2 ALTSVC frame (RFC 7838, section 4), which advertises without waiting for a request.
// =================================================================================================

namespace
{

nghttp2_nv nv(std::string_view name, std::string_view value)
{
   return {const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name.data())),
           const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value.data())), name.size(),
           value.size(), NGHTTP2_NV_FLAG_NONE};
}

/**
 * A bare HTTP/2 server, driven by nghttp2 by hand, that sends one ALTSVC frame right after its
 * SETTINGS and answers every request with an empty 200.
 *
 * anyhttp's own server has no reason to send one -- it puts the very same thing into a header
 * field of every response -- so this is the only way to get the client's frame path under test.
 */
awaitable<void> serve_h2_with_altsvc(tcp::socket socket, std::string origin, std::string value)
{
   auto callbacks = std::invoke([] {
      nghttp2_session_callbacks* cbs;
      nghttp2_session_callbacks_new(&cbs);
      nghttp2_session_callbacks_set_on_frame_recv_callback(
         cbs, [](nghttp2_session* session, const nghttp2_frame* frame, void*) -> int {
            if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST)
            {
               std::array nva{nv(":status", "200"), nv("content-length", "0")};
               nghttp2_submit_response2(session, frame->hd.stream_id, nva.data(), nva.size(),
                                        nullptr);
            }
            return 0;
         });
      return std::unique_ptr<nghttp2_session_callbacks, decltype(&nghttp2_session_callbacks_del)>{
         cbs, nghttp2_session_callbacks_del};
   });

   nghttp2_session* raw = nullptr;
   nghttp2_session_server_new(&raw, callbacks.get(), nullptr);
   auto session =
      std::unique_ptr<nghttp2_session, decltype(&nghttp2_session_del)>{raw, nghttp2_session_del};

   nghttp2_settings_entry settings{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100};
   nghttp2_submit_settings(raw, NGHTTP2_FLAG_NONE, &settings, 1);

   //
   // On stream 0, the frame has to name the origin it is about (RFC 7838, section 4).
   //
   EXPECT_EQ(nghttp2_submit_altsvc(raw, NGHTTP2_FLAG_NONE, 0,
                                   reinterpret_cast<const uint8_t*>(origin.data()), origin.size(),
                                   reinterpret_cast<const uint8_t*>(value.data()), value.size()),
             0);

   std::array<uint8_t, 16_k> buffer;
   for (;;)
   {
      for (;;)
      {
         const uint8_t* data = nullptr;
         auto n = nghttp2_session_mem_send2(raw, &data);
         if (n <= 0)
            break;
         if (auto [ec, written] = co_await async_write(socket, asio::buffer(data, n), as_tuple); ec)
            co_return;
      }

      auto [ec, n] = co_await socket.async_read_some(asio::buffer(buffer), as_tuple);
      if (ec || nghttp2_session_mem_recv2(raw, buffer.data(), n) < 0)
         co_return;
   }
}

} // namespace

class AltSvcFrame : public Server
{
};

INSTANTIATE_TEST_SUITE_P(AltSvcFrame, AltSvcFrame, Values(anyhttp::Protocol::h2), NameGenerator);

TEST_P(AltSvcFrame, WHEN_an_altsvc_frame_arrives_THEN_the_next_connection_uses_it)
{
   //
   // The bare HTTP/2 server gets an endpoint of its own, and points at the HTTP/3 endpoint of the
   // fixture's server, which is the one that can actually answer over QUIC.
   //
   tcp::acceptor acceptor(context, tcp::endpoint(ip::make_address("127.0.0.2"), 0));
   auto origin = std::format("http://127.0.0.2:{}", acceptor.local_endpoint().port());
   auto value = std::format("h3=\":{}\"", server->local_endpoint().port());

   co_spawn(
      context,
      [&]() -> awaitable<void> {
         auto socket = co_await acceptor.async_accept();
         co_await serve_h2_with_altsvc(std::move(socket), origin, value);
      },
      [](const std::exception_ptr& ex) { logi("bare HTTP/2 server: {}", what(ex)); });

   boost::urls::url target{"http://127.0.0.2/echo"};
   target.set_port_number(acceptor.local_endpoint().port());
   client::Client client(context.get_executor(),
                         {.url = target, .protocol = Protocol::h2, .follow_alt_svc = true});

   co_spawn(
      context,
      [&]() -> awaitable<void> {
         //
         // The response itself carries no "Alt-Svc" -- everything the client learns here, it learns
         // from the frame that arrived before the request was even sent.
         //
         auto first = co_await (co_await client.async_connect()).async_get(target);
         EXPECT_EQ(first.result_int(), 200);
         EXPECT_EQ(first[http::field::alt_svc], "");

         auto second = co_await (co_await client.async_connect()).async_get(target);
         EXPECT_EQ(second.result_int(), 200);
         EXPECT_TRUE(served_over_http3(second)) << "the second connection is not HTTP/3";
      },
      [&](const std::exception_ptr& ex) {
         EXPECT_FALSE(ex) << what(ex);
         acceptor.close();
         server.reset();
      });

   run();
}

// =================================================================================================
