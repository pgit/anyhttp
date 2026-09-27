#include "test_fixtures.hpp"

using namespace testing;

namespace http = boost::beast::http;

// =================================================================================================

//
// The default fixture listens on 127.0.0.2, so everything else runs over IPv4. These put the
// server on an IPv6 socket: for HTTP/3 that means IPV6_RECVPKTINFO and the AF_INET6 branch of
// msghdr_get_local_addr(), which the reply path depends on to send from the right address.
//
class IPv6 : public ClientAsync
{
protected:
   IPv6() { url = boost::urls::url{"http://[::1]/custom"}; }

   void configure_server(server::Config& config) override { config.listen_address = "::1"; }

   void respond_with(std::string body)
   {
      requestHandler = [body = std::move(body)](server::Request request,
                                                server::Response response) -> awaitable<void> {
         co_await drain(request);
         co_await response.async_submit(200, fields({{"Content-Length", body.size()}}));
         co_await response.async_write_eof(asio::buffer(body));
      };
   }
};

INSTANTIATE_TEST_SUITE_P(IPv6, IPv6,
                         Values(anyhttp::Protocol::h1, anyhttp::Protocol::h2,
                                anyhttp::Protocol::h3),
                         NameGenerator);

TEST_P(IPv6, WHEN_server_listens_on_ipv6_loopback_THEN_get_succeeds)
{
   respond_with("Hello, IPv6!");
   clientSession = [this](Session session) -> awaitable<void> {
      auto message = co_await session.async_get(url);
      EXPECT_EQ(message.result(), http::status::ok);
      EXPECT_EQ(message.body(), "Hello, IPv6!");
   };
}

// -------------------------------------------------------------------------------------------------

//
// A dual-stack listener on "::" reached over IPv4. The socket is AF_INET6 but the peer is not: the
// kernel reports the local address as v4-mapped (::ffff:127.0.0.1) through IPV6_PKTINFO, and the
// response has to go back out from that.
//
class DualStack : public IPv6
{
protected:
   DualStack() { url = boost::urls::url{"http://127.0.0.1/custom"}; }

   void configure_server(server::Config& config) override { config.listen_address = "::"; }
};

INSTANTIATE_TEST_SUITE_P(DualStack, DualStack,
                         Values(anyhttp::Protocol::h1, anyhttp::Protocol::h2,
                                anyhttp::Protocol::h3),
                         NameGenerator);

TEST_P(DualStack, WHEN_ipv6_server_is_reached_over_ipv4_THEN_get_succeeds)
{
   respond_with("Hello, IPv4!");
   clientSession = [this](Session session) -> awaitable<void> {
      auto message = co_await session.async_get(url);
      EXPECT_EQ(message.result(), http::status::ok);
      EXPECT_EQ(message.body(), "Hello, IPv4!");
   };
}
