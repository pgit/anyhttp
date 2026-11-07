//
// What Session::Impl does for a backend that does not do it itself. Only the client hands out a
// Session, and each client backend has an orderly end of its own, so these defaults are not
// reachable over a real connection: a session with nothing behind it stands in for one.
//
#include "test_fixtures.hpp"

#include "anyhttp/session_impl.hpp"

#include <memory>
#include <string_view>
#include <tuple>
#include <utility>

using namespace testing;

namespace
{

// =================================================================================================

/// A session that implements nothing beyond what it must, to see what the base class adds.
class BareSession : public Session::Impl
{
public:
   explicit BareSession(Executor executor) : executor_(std::move(executor)) {}

   Executor get_executor() const noexcept override { return executor_; }
   Protocol protocol() const noexcept override { return Protocol::h1; }
   void async_submit(SubmitHandler&& handler, std::string_view, boost::urls::url,
                     const Fields&) override
   {
      std::move(handler)(make_error_code(errc::operation_not_supported), client::Request{});
   }
   Task<void> do_session(Buffer&&) override { co_return; }
   void destroy() noexcept override { destroyed = true; }

   bool destroyed = false;

private:
   Executor executor_;
};

Task<void> shut_down(Session& session, error_code& result)
{
   std::tie(result) = co_await session.shutdown();
}

} // namespace

// =================================================================================================

TEST(SessionImpl, WHEN_backend_has_no_orderly_end_THEN_shutdown_is_not_supported)
{
   IoContext context;
   auto impl = std::make_shared<BareSession>(context.get_executor());
   Session session{impl};

   error_code result = errors::canceled;
   launch(context.get_executor(), shut_down(session, result));
   context.run();

   EXPECT_EQ(result, errc::operation_not_supported);
   EXPECT_FALSE(impl->destroyed) << "not supported is not a reason to end it the abrupt way";
}

// =================================================================================================
