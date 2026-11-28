#pragma once

//
// Fixtures and helpers shared by the test_*.cpp files. The fixtures themselves -- Server, Client
// and ClientAsync -- are runtime-specific, in test_fixtures_asio.hpp and test_fixtures_corosio.hpp.
//
#include "test_fixtures_common.hpp"

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
auto check(results<T...>&& result)
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
