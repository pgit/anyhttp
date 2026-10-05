#pragma once

//
// Fixtures and helpers shared by the test_*.cpp files. The fixtures themselves -- Server, Client
// and ClientAsync -- are runtime-specific, in test_fixtures_asio.hpp and test_fixtures_corosio.hpp.
//
#include "anyhttp/client.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/request_handlers.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/session.hpp"
#include "anyhttp/utils.hpp"

#include <boost/beast/http/error.hpp>
#include <boost/url/url.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace std::string_view_literals;
using namespace std::chrono_literals;

namespace rv = std::ranges::views;

using namespace anyhttp;

// =================================================================================================

/// Returns HTTP11, HTTP2 or HTTP3 depending on the protocol.
static std::string NameGenerator(const testing::TestParamInfo<anyhttp::Protocol>& info)
{
   return to_string(info.param);
}

/// The protocols the parametrized shared tests run with.
inline std::vector<anyhttp::Protocol> protocols()
{
   return {anyhttp::Protocol::h1, anyhttp::Protocol::h2, anyhttp::Protocol::h3};
}

static void setup_logging()
{
#if defined(GITHUB_ACTIONS)
   spdlog::set_level(spdlog::level::warn);
#elif defined(NDEBUG)
   spdlog::set_level(spdlog::level::info);
#else
   spdlog::set_level(spdlog::level::debug);
#endif
}

// =================================================================================================

#if ANYHTTP_COROSIO
#include "test_fixtures_corosio.hpp"
#else
#include "test_fixtures_asio.hpp"
#endif

// =================================================================================================
// Shared by both runtimes
// =================================================================================================

/**
 * The value of an operation's result, throwing its error -- what ASIO's default completion token
 * does, for the tests written in the coroutine spelling both runtimes have:
 * <tt>auto request = check(co_await session.submit(url));</tt>
 */
template <typename... T>
auto check(std::tuple<error_code, T...>&& result)
{
   if (auto& ec = std::get<0>(result))
      throw_error(ec);
   if constexpr (sizeof...(T) == 1)
      return std::move(std::get<1>(result));
   else if constexpr (sizeof...(T) > 1)
      return std::apply([](auto&&, auto&&... rest) { return std::tuple{std::move(rest)...}; },
                        std::move(result));
}

/// Awaits \p task and yields what it threw, if anything.
inline Task<std::exception_ptr> caught(Task<void> task)
{
   try
   {
      co_await std::move(task);
   }
   catch (...)
   {
      co_return std::current_exception();
   }
   co_return nullptr;
}
