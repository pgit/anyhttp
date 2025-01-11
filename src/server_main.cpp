#include "anyhttp/request_handlers.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/utils.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/program_options.hpp>

#include <fmt/ostream.h>

#include <iostream>
#include <range/v3/view/take_while.hpp>
#include <ranges>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace rv = std::ranges::views;

using namespace std::chrono_literals;
using namespace boost::asio;
using namespace anyhttp;
using namespace anyhttp::server;
namespace po = boost::program_options;

int main(int argc, char* argv[])
{
   // Define program options
   po::options_description desc("Allowed options");
   desc.add_options()("help,h", "produce help message")(
      "verbose,v", po::bool_switch()->default_value(false), "enable verbose logging")(
      "threads,t", po::value<size_t>()->default_value(1), "number of threads to run");

   po::variables_map vm;
   po::store(po::parse_command_line(argc, argv, desc), vm);
   po::notify(vm);

   if (vm.count("help"))
   {
      std::cout << desc << "\n";
      return 1;
   }

   size_t num_threads = vm["threads"].as<size_t>();
   if (num_threads == 0)
   {
      fmt::println(stderr, "Number of threads must be greater than 0");
      return 1;
   }

   io_context context(num_threads);
   auto executor = context.get_executor();
   auto config = Config{.port = 8080, .use_strand = num_threads > 1};
   auto server = std::make_optional<Server>(executor, config);

   signal_set signals(context, SIGINT, SIGTERM);
   signals.async_wait(
      [&](auto, auto)
      {
         fmt::println(" INTERRUPTED");
         logw("interrupt");
         server.reset();
      });

   server->setRequestHandlerCoro(
      [](server::Request request, server::Response response) -> awaitable<void>
      {
         if (request.url().path() == "/echo")
            return echo(std::move(request), std::move(response));
         else if (request.url().path() == "/")
            return h2spec(std::move(request), std::move(response));
         else
            return not_found(std::move(response));
      });

   auto threads = rv::iota(0) | rv::take(num_threads > 0 ? num_threads - 1 : 0) |
                  rv::transform([&](int) { return std::thread([&] { context.run(); }); }) |
                  ranges::to<std::vector>();

   context.run();

   for (auto& thread : threads)
      thread.join();

   return 0;
}
