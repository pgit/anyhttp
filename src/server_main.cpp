#include "anyhttp/file_handler.hpp"
#include "anyhttp/request_handlers.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/utils.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/program_options.hpp>

#include <algorithm>
#include <expected>
#include <iostream>
#include <print>
#include <ranges>

#include <sys/ioctl.h>
#include <unistd.h>

namespace rv = std::ranges::views;

using namespace std::chrono_literals;
using namespace boost::asio;
using namespace anyhttp;
namespace po = boost::program_options;

struct Config
{
   size_t verbose = 0;
   size_t threads = 1;
   server::Config server{.port = 8080};
};

std::expected<Config, int> parseConfig(int argc, char* argv[])
{
   Config config;

   // Define program options, wrapping the help text at the terminal's width (it goes to stderr)
   winsize ws{};
   unsigned columns = ::ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 40
                         ? ws.ws_col
                         : po::options_description::m_default_line_length;
   po::options_description desc("Allowed options", columns, columns / 2);
   auto opts = desc.add_options();
   opts("help,h", "produce help message");
   opts("verbose,v", po::value<std::vector<std::string>>()->zero_tokens()->composing(),
        "enable verbose logging (repeat for trace level)");
   opts("threads,t", po::value(&config.threads)->default_value(1), "number of threads to run");
   opts("port,p", po::value(&config.server.port)->default_value(config.server.port),
        "listening port");
   opts("drop-rx", po::value(&config.server.drop_rate_rx)->default_value(0.0),
        "HTTP/3 testing: probability (0.0 .. 1.0) of dropping a received QUIC packet");
   opts("drop-tx", po::value(&config.server.drop_rate_tx)->default_value(0.0),
        "HTTP/3 testing: probability (0.0 .. 1.0) of dropping a QUIC packet before sending it");
   opts("disable-gro", po::bool_switch(&config.server.disable_gro),
        "HTTP/3 benchmarking: don't enable UDP_GRO (receive offload) on the UDP socket");
   opts("disable-gso", po::bool_switch(&config.server.disable_gso),
        "HTTP/3 benchmarking: don't use UDP_SEGMENT (send offload), one sendto() per packet");
   opts("max-header-size",
        po::value(&config.server.max_header_size)->default_value(config.server.max_header_size),
        "largest request header section accepted, in bytes (answered with 431 if exceeded)");

   po::variables_map vm;
   try
   {
      auto parsed = po::parse_command_line(argc, argv, desc);
      po::store(parsed, vm);
      po::notify(vm);

      // 'verbose' takes no argument, so its parsed value is always empty -- count occurrences
      config.verbose = std::ranges::count_if(parsed.options, [](const po::option& option)
                                             { return option.string_key == "verbose"; });
   }
   catch (const po::error& error)
   {
      std::println(std::cerr, "{}", error.what());
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
      std::println(std::cerr, "number of threads must be greater than 0");
      return std::unexpected(1);
   }

   for (auto [name, rate] : {std::pair{"drop-rx", config.server.drop_rate_rx},
                             std::pair{"drop-tx", config.server.drop_rate_tx}})
   {
      if (rate < 0.0 || rate > 1.0)
      {
         std::println(std::cerr, "--{} must be between 0.0 and 1.0", name);
         return std::unexpected(1);
      }
   }

   return {std::move(config)};
}

int main(int argc, char* argv[])
{
   auto config = parseConfig(argc, argv);
   if (!config)
      return config.error();

   if (config->verbose >= 2)
      spdlog::set_level(spdlog::level::trace);
   else if (config->verbose)
      spdlog::set_level(spdlog::level::debug);
   else
      spdlog::set_level(spdlog::level::info);

   io_context context(config->threads);
   auto executor = context.get_executor();
   config->server.use_strand = config->threads > 1;
   auto server = std::make_optional<server::Server>(executor, config->server);

   signal_set signals(context, SIGINT, SIGTERM);
   signals.async_wait([&](boost::system::error_code error, auto signal)
   {
      std::println(" INTERRUPTED (signal {})", signal);
      logw("interrupt");
      server.reset();
   });

   server->setRequestHandler(
      [](server::Request request, server::Response response) -> awaitable<void>
   {
      std::string path = request.url().path();
      if (path == "/echo")
         co_await echo(std::move(request), std::move(response));
      else if (path == "/generate")
         co_await generate(std::move(request), std::move(response));
      else if (path == "/dump")
         co_await dump(std::move(request), std::move(response));
      else if (path == "/dump space")
         co_await dump(std::move(request), std::move(response));
      else if (path == "/discard")
         co_return;
      else if (path == "/test" || path.starts_with("/test/"))
         co_await serve_file(std::move(request), std::move(response), "test", "/test");
      else if (path == "/eat_request")
         co_await eat_request(std::move(request), std::move(response));
      else if (path == "/upload")
      {
         // Unlike eat_request, respond only after the whole body is in: clients such as h2load
         // stop uploading as soon as the response is complete.
         co_await drain(request);
         co_await response.async_submit(200, {});
         co_await response.async_write_eof();
      }
      else if (path == "/" || path == "/h2spec")
         co_await h2spec(std::move(request), std::move(response));
      else
         co_await not_found(std::move(response));
   });

   auto threads = rv::iota(0) | rv::take(config->threads > 0 ? config->threads - 1 : 0) |
                  rv::transform([&](size_t) { return std::thread([&] { context.run(); }); }) |
                  std::ranges::to<std::vector>();

   if (config->verbose && config->threads == 1)
      run(context);
   else
      context.run();

   for (auto& thread : threads)
      thread.join();

   return 0;
}
