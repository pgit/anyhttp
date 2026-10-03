#include "anyhttp/file_handler.hpp"
#include "anyhttp/request_handlers.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/utils.hpp"

#if ANYHTTP_COROSIO
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/signal_set.hpp>
#else
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#endif

#include <boost/program_options.hpp>

#include <algorithm>
#include <expected>
#include <iostream>
#include <print>

#include <sys/ioctl.h>
#include <unistd.h>

using namespace std::chrono_literals;
using namespace anyhttp;
namespace po = boost::program_options;

#if ANYHTTP_COROSIO
using IoContext = boost::corosio::io_context;
using SignalSet = boost::corosio::signal_set;
#else
using IoContext = boost::asio::io_context;
using SignalSet = boost::asio::signal_set;
#endif

namespace
{

struct Config
{
   size_t verbose = 0;
   size_t threads = 1;
   bool independent = false;
   server::Config server{.port = 8080};
#if ANYHTTP_COROSIO
   boost::corosio::io_context_options context;
#endif
};

} // namespace

static std::expected<Config, int> parse_config(int argc, char* argv[])
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
   opts("independent,i", po::bool_switch(&config.independent),
        "with --threads above 1: run an I/O context and a server of its own on each thread, "
        "sharing the port through SO_REUSEPORT, instead of one context on all threads with a "
        "strand per connection");
   opts("port,p", po::value(&config.server.port)->default_value(config.server.port),
        "listening port");
   opts("drop-rx", po::value(&config.server.drop_rate_rx)->default_value(0.0),
        "HTTP/3 testing: probability (0.0 .. 1.0) of dropping a received QUIC packet");
   opts("drop-tx", po::value(&config.server.drop_rate_tx)->default_value(0.0),
        "HTTP/3 testing: probability (0.0 .. 1.0) of dropping a QUIC packet before sending it");
   opts("disable-gro", po::bool_switch(&config.server.disable_gro),
        "HTTP/3 benchmarking: don't enable UDP_GRO (receive offload) on the UDP socket");
   opts("disable-gso", po::bool_switch(&config.server.disable_gso),
        "HTTP/3 benchmarking: don't use UDP_SEGMENT (send offload), one sendmsg() per packet");
   opts("max-header-size",
        po::value(&config.server.max_header_size)->default_value(config.server.max_header_size),
        "largest request header section accepted, in bytes (answered with 431 if exceeded)");
   opts("cert",
        po::value(&config.server.tls_certificate_chain)
           ->default_value(config.server.tls_certificate_chain),
        "PEM file with the server certificate, followed by its intermediates");
   opts("key",
        po::value(&config.server.tls_private_key)->default_value(config.server.tls_private_key),
        "PEM file with the server's private key");
   long alt_svc_max_age = config.server.alt_svc_max_age.count();
   opts("alt-svc-max-age", po::value(&alt_svc_max_age)->default_value(alt_svc_max_age),
        "how long clients may remember the HTTP/3 endpoint advertised as 'Alt-Svc' over HTTP/1.1 "
        "and HTTP/2, in seconds (0 advertises nothing)");
#if ANYHTTP_COROSIO
   opts(
      "inline-budget",
      po::value(&config.context.inline_budget_max)->default_value(config.context.inline_budget_max),
      "ceiling of corosio's adaptive inline budget: how many I/O operations that are ready at "
      "once complete without a post before one is posted (0 posts all). With --threads above 1 "
      "and all budgets at their defaults, corosio posts all");
   opts(
      "unassisted-budget",
      po::value(&config.context.unassisted_budget)->default_value(config.context.unassisted_budget),
      "the inline budget when other handlers are queued (no other thread takes them), in place "
      "of the adaptive one; capped by --inline-budget");
#endif

   po::variables_map vm;
   try
   {
      auto parsed = po::parse_command_line(argc, argv, desc);
      po::store(parsed, vm);
      po::notify(vm);

      config.server.alt_svc_max_age = std::chrono::seconds{std::max(0L, alt_svc_max_age)};

      // 'verbose' takes no argument, so its parsed value is always empty -- count occurrences
      config.verbose = std::ranges::count_if(
         parsed.options, [](const po::option& option) { return option.string_key == "verbose"; });
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

/// Waits for one of \p signals, then stops \p servers, each on its own executor.
static Task<void> stop_on_signal(SignalSet& signals,
                                 std::vector<std::optional<server::Server>>& servers)
{
#if ANYHTTP_COROSIO
   auto [ec, signal] = co_await signals.wait();
#else
   auto [ec, signal] = co_await signals.async_wait(boost::asio::as_tuple);
#endif
   if (ec)
      co_return;

   std::println(" INTERRUPTED (signal {})", signal);
   logw("interrupt");
   for (auto& server : servers)
      dispatch_to(server->get_executor(), [&server] { server.reset(); });
}

static Task<void> handle_request(server::Request request, server::Response response)
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
      if (auto [ec] = co_await response.submit(200, {}); !ec)
         co_await response.write_eof();
   }
   else if (path == "/" || path == "/h2spec")
      co_await h2spec(std::move(request), std::move(response));
   else
      co_await not_found(std::move(response));
}

int main(int argc, char* argv[])
{
   auto config = parse_config(argc, argv);
   if (!config)
      return config.error();

   if (config->verbose >= 2)
      spdlog::set_level(spdlog::level::trace);
   else if (config->verbose)
      spdlog::set_level(spdlog::level::debug);
   else
      spdlog::set_level(spdlog::level::info);

   //
   // Either one I/O context, run by all threads, with a strand per connection, or (--independent)
   // one context per thread, each with a server of its own: they share the port through
   // SO_REUSEPORT, and nothing is shared between them but the signal handler.
   //
   size_t num_contexts = config->independent ? config->threads : 1;
   size_t threads_per_context = config->independent ? 1 : config->threads;
   config->server.use_strand = threads_per_context > 1;
   config->server.reuse_port = num_contexts > 1;

   std::vector<std::unique_ptr<IoContext>> contexts;
   std::vector<std::optional<server::Server>> servers;
   for (size_t i = 0; i < num_contexts; ++i)
   {
#if ANYHTTP_COROSIO
      auto& context =
         *contexts.emplace_back(std::make_unique<IoContext>(config->context, threads_per_context));
#else
      auto& context = *contexts.emplace_back(std::make_unique<IoContext>(threads_per_context));
#endif
      auto& server = servers.emplace_back(std::in_place, context.get_executor(), config->server);
      server->on_request(handle_request);
      if (config->server.port == 0) // the others join the port the first one got
         config->server.port = server->local_endpoint().port();
   }

   auto& context = *contexts.front();
   SignalSet signals(context, SIGINT, SIGTERM);
   launch(context.get_executor(), stop_on_signal(signals, servers));

   std::vector<std::thread> threads;
   for (auto& other : contexts)
      for (size_t i = 0; i < threads_per_context; ++i)
         if (&other != &contexts.front() || i > 0)
            threads.emplace_back([&context = *other] { context.run(); });

   if (config->verbose && config->threads == 1)
      run(context);
   else
      context.run();

   for (auto& thread : threads)
      thread.join();

   return 0;
}
