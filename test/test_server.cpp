#include <anyhttp/common.hpp>
#include <anyhttp/server.hpp>

#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/deferred.hpp>

#include <boost/process.hpp>

#include <exception>
#include <gtest/gtest.h>

#include <fmt/ostream.h>

namespace bp = boost::process;

using namespace boost::asio;
namespace asio = boost::asio;

using tcp = asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>
using asio::as_tuple;
using asio::awaitable;
using asio::co_spawn;
using asio::deferred;

std::string what(const std::exception_ptr& ptr)
{
   std::string result;
   try
   {
      std::rethrow_exception(ptr);
   }
   catch (std::exception& ex)
   {
      result = fmt::format("exception: {}", ex.what());
   }
   return result;
}

using namespace anyhttp::server;

namespace method
{
};

TEST(Server, HelloWorld)
{
   boost::asio::io_context context;

   auto config = anyhttp::server::Config{.port = 0};
   auto server = std::make_optional<anyhttp::server::Server>(context.get_executor(), config);

#if 0
   server->setRequestHandler([](Request request, Response response) -> void {
     
   });
#endif

   // std::cout << bp::search_path("curl") << std::endl; // FIXME: why does this segfault

   auto nghttp = bp::filesystem::path("/workspaces/nghttp2/install/bin/nghttp");
   auto url = fmt::format("http://127.0.0.1:{}", server->local_endpoint().port());

   bp::async_pipe out(context);
   bp::child child(
      nghttp, "-d", "CMakeLists.txt", url, bp::std_out > out, bp::std_err > bp::null,
      bp::on_exit = [](int exit, const std::error_code& ec)
      { fmt::println("exit={}, ec={}", exit, ec.message()); });

   co_spawn(
      context,
      [&]() -> awaitable<void>
      {
         std::vector<char> buf(4096);
         for (;;)
         {
            auto [ex, nread] =
               co_await asio::async_read(out, asio::buffer(buf), as_tuple(deferred));
            // fmt::println("read {} bytes, {}", nread, ex.message());
            fmt::print("{}", std::string_view(buf.data(), nread));
            if (ex)
               break;
         }

         child.wait(); // FIXME: this is sync
         logi("exit_code={}", child.exit_code());
         server.reset();
      },
      detached);

   logi("running IO context...");
   context.run();
   logi("running IO context... done");
}