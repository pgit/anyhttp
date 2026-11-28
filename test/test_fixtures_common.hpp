#pragma once

//
// What the fixtures of test_fixtures.hpp share, including test_fixtures_asio.hpp and
// test_fixtures_corosio.hpp: each of these includes this first, so that it compiles on its own.
//
#include "anyhttp/client.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/request_handlers.hpp"
#include "anyhttp/server.hpp"
#include "anyhttp/session.hpp"
#include "anyhttp/utils.hpp"

#include <boost/url/url.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>

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
