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

// #include <range/v3/view/take_while.hpp>
// #include <range/v3/view/transform.hpp>
#include <ranges>

#include <fmt/ostream.h>

#include <expected>
#include <iostream>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace rv = std::ranges::views;

using namespace std::chrono_literals;
using namespace boost::asio;
using namespace anyhttp;
namespace po = boost::program_options;

struct Config
{
   bool verbose = false;
   size_t threads = 1;
   server::Config server{.port = 8080};
};

std::expected<Config, int> parseConfig(int argc, char* argv[])
{
   Config config;

   // Define program options
   po::options_description desc("Allowed options");
   auto opts = desc.add_options();
   opts("help,h", "produce help message");
   opts("verbose,v", po::bool_switch(&config.verbose)->default_value(false),
        "enable verbose logging");
   opts("threads,t", po::value(&config.threads)->default_value(1), "number of threads to run");
   opts("port,p", po::value(&config.server.port)->default_value(config.server.port),
        "listening port");

   po::variables_map vm;
   try
   {
      po::store(po::parse_command_line(argc, argv, desc), vm);
      po::notify(vm);
   }
   catch (const po::error& error)
   {
      fmt::println(stderr, "{}", error.what());
      return std::unexpected(-1);
   }

   if (vm.count("help"))
   {
      std::cerr << desc << "\n";
      return std::unexpected(1);
   }

   size_t num_threads = vm["threads"].as<size_t>();
   if (num_threads == 0)
   {
      fmt::println(stderr, "number of threads must be greater than 0");
      return std::unexpected(1);
   }

   return {std::move(config)};
}

int main(int argc, char* argv[])
{
   auto config = parseConfig(argc, argv);
   if (!config)
      return config.error();

   if (config->verbose)
      spdlog::set_level(spdlog::level::debug);
   else
      spdlog::set_level(spdlog::level::info);

   io_context context(config->threads);
   auto executor = context.get_executor();
   config->server.use_strand = config->threads > 1;
   auto server = std::make_optional<server::Server>(executor, config->server);

   signal_set signals(context, SIGINT, SIGTERM);
   signals.async_wait(
      [&](boost::system::error_code error, auto signal)
      {
         fmt::println(" INTERRUPTED (signal {})", signal);
         logw("interrupt");
         server.reset();
      });

   server->setRequestHandlerCoro(
      [](server::Request request, server::Response response) -> awaitable<void>
      {
         if (request.url().path() == "/echo")
            return echo(std::move(request), std::move(response));
         else if (request.url().path() == "/eat_request")
            return eat_request(std::move(request), std::move(response));
         else if (request.url().path() == "/")
            return h2spec(std::move(request), std::move(response));
         else
            return not_found(std::move(response));
      });

   auto threads = rv::iota(0) | rv::take(config->threads > 0 ? config->threads - 1 : 0) |
                  rv::transform([&](size_t) { return std::thread([&] { context.run(); }); }) |
                  ranges::to<std::vector>();

   if (config->verbose && config->threads == 1)
      run(context);
   else
      context.run();

   for (auto& thread : threads)
      thread.join();

   return 0;
}
