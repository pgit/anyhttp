#pragma once

#include "common.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>

#include <concepts>
#include <memory>
#include <optional>
#include <string>

namespace anyhttp
{

// =================================================================================================

/**
 * The reading half of a message, as a handle: what a \c server::Request and a \c client::Response
 * have in common, and all that an operation which only consumes a body -- \c drain() -- needs to
 * see of either.
 *
 * This is the PIMPL handle itself, not a view onto one: it owns its share of the implementation,
 * and \c server::Request and \c client::Response derive from it rather than holding a pointer of
 * their own. They add no data members, so moving one into a \c Reader ("slicing") is a sound and
 * useful thing to do -- it hands off the reading half with its ownership intact, to be drained by
 * a coroutine that has no business with the rest of the message. That is also why the destructor
 * is not virtual: these are handles, never owned through a base pointer.
 */
class Reader
{
public:
   /// The implementation, defined in \c reader_impl.hpp: what a protocol backend implements to
   /// be read from. Only code that implements or narrows one needs to see it.
   class Impl;

   using executor_type = Executor;

   explicit Reader(std::shared_ptr<Impl> impl) noexcept;
   Reader(Reader&&) noexcept;
   Reader& operator=(Reader&&) noexcept;
   ~Reader();

   /// Releases the implementation, as the destructor does. Reading afterwards fails.
   void reset() noexcept;

   explicit constexpr operator bool() const noexcept { return static_cast<bool>(impl_); }

   /// The executor of the session this message belongs to, or an empty one after \c reset().
   executor_type get_executor() const noexcept;

   /// The \c [proto:address:port.stream] tag the library's log lines carry for this message,
   /// without the brackets, or an empty string after \c reset().
   std::string log_prefix() const;

   /// What the incoming message announced as its body length, if it announced one.
   std::optional<size_t> content_length() const noexcept;

   /**
    * Reads a part of the incoming body.
    *
    * The end of the body is reported as \c asio::error::eof with zero bytes, as ASIO does
    * everywhere else, and so is every read after it. A body cut short by a reset stream or a lost
    * connection completes with \c http::error::partial_message instead.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadSome) CompletionToken = DefaultCompletionToken>
   auto async_read_some(asio::mutable_buffer buffer, CompletionToken&& token = CompletionToken())
   {
      //
      // Binding an executor to the initiating function lets tokens that need one -- the timer
      // behind cancel_after -- find it here, see Writer.
      //
      return asio::async_initiate<CompletionToken, ReadSome>(
         asio::bind_executor(asio::get_associated_executor(token, get_executor()),
                             [this](ReadSomeHandler handler, asio::mutable_buffer buffer) { //
                                async_read_some_any(buffer, std::move(handler));
                             }),
         token, buffer);
   }

   /**
    * \overload
    *
    * FIXME: When given an actual sequence of buffers, this fills only the first non-empty one.
    */
   template <typename Buffers,
             BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadSome) CompletionToken = DefaultCompletionToken>
      requires(asio::is_mutable_buffer_sequence<Buffers>::value &&
               !std::convertible_to<const Buffers&, asio::mutable_buffer>)
   auto async_read_some(const Buffers& buffers, CompletionToken&& token = CompletionToken())
   {
      for (auto& buffer : buffers)
         if (buffer.size() > 0)
            return async_read_some(asio::mutable_buffer(buffer),
                                   std::forward<CompletionToken>(token));

      return async_read_some(asio::mutable_buffer{}, std::forward<CompletionToken>(token));
   }

protected:
   /// The implementation, for the derived handle to narrow to its own \c Impl. Never null.
   Impl& pimpl() const noexcept { return *impl_; }

private:
   void async_read_some_any(asio::mutable_buffer buffer, ReadSomeHandler&& handler);

   std::shared_ptr<Impl> impl_;
};

static_assert(AsyncReadStream<Reader>);

// =================================================================================================

} // namespace anyhttp
