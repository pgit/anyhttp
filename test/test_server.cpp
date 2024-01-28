#include <anyhttp/common.hpp>
#include <anyhttp/server.hpp>

#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/deferred.hpp>

#include <boost/process.hpp>
#include <boost/process/args.hpp>

#include <boost/process/args.hpp>
#include <gtest/gtest.h>

#include <fmt/ostream.h>

using namespace std::chrono_literals;
namespace bp = boost::process;
using namespace boost::asio;
namespace asio = boost::asio;

using tcp = asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>
using asio::as_tuple;
using asio::awaitable;
using asio::co_spawn;
using asio::deferred;

using namespace anyhttp;
using namespace anyhttp::server;

class Echo : public testing::Test
{
protected:
   void SetUp() override
   {
      auto config = anyhttp::server::Config{.port = 0};
      server.emplace(context.get_executor(), config);
      server->setRequestHandlerCoro(
         [](Request request, Response response) -> awaitable<void>
         {
            response.write_head(200, {});
            for (;;)
            {
               auto buffer = co_await request.async_read_some(deferred);
               auto len = buffer.size();
               if (len == 0)
               {
                  auto timer = steady_timer(request.executor());
                  timer.expires_after(50ms);
                  co_await timer.async_wait(deferred);
               }
               co_await response.async_write(std::move(buffer), deferred);

               // if (len == 0)
               // break;
            }
            co_return;
         });
   }

   awaitable<std::string> spawn_process(bp::filesystem::path path, std::vector<std::string> args)
   {
      bp::async_pipe out(context);
      bp::child child(
         path, std::move(args), bp::std_out > out, bp::std_err > bp::null,
         bp::on_exit = [](int exit, const std::error_code& ec) { //
            fmt::println("exit={}, ec={}", exit, ec.message());
         });

      std::string result;
      std::vector<char> buf(4096);
      for (;;)
      {
         auto [ex, nread] = co_await asio::async_read(out, asio::buffer(buf), as_tuple(deferred));
         result += std::string_view(buf.data(), nread);
         if (ex)
            break;
      }

      child.wait(); // FIXME: this is sync
      logi("exit_code={}", child.exit_code());
      server.reset();
      co_return result;
   }

   auto spawn(bp::filesystem::path path, std::vector<std::string> args)
   {
      return co_spawn(context, spawn_process(std::move(path), std::move(args)), use_future);
   }

   boost::asio::io_context context;
   std::optional<anyhttp::server::Server> server;
};

TEST_F(Echo, NGHTTP)
{
   auto url = fmt::format("http://127.0.0.1:{}", server->local_endpoint().port());
   auto future = spawn("/workspaces/nghttp2/install/bin/nghttp", {"-d", "CMakeLists.txt", url});
   context.run();
   EXPECT_EQ(future.get().size(), 1160);
}

TEST_F(Echo, Curl)
{
   auto url = fmt::format("http://127.0.0.1:{}", server->local_endpoint().port());
   auto future =
      spawn("/usr/bin/curl", {"--http2-prior-knowledge", "--data-binary", "@CMakeLists.txt", url});
   context.run();
   EXPECT_EQ(future.get().size(), 1160);
}

TEST_F(Echo, CurlHttp11)
{
   auto url = fmt::format("http://127.0.0.1:{}", server->local_endpoint().port());
   auto future = spawn("/usr/bin/curl", {"--data-binary", "@CMakeLists.txt", url});
   context.run();
   EXPECT_EQ(future.get().size(), 1160);
}
