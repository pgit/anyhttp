#pragma once

#include "common.hpp"

#include <boost/asio/buffer.hpp>

#if ANYHTTP_COROSIO
#include <boost/capy/concept/const_buffer_sequence.hpp>
#include <boost/capy/concept/write_stream.hpp>
#else
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#endif

#include <memory>
#include <optional>
#include <string>

namespace anyhttp
{

// =================================================================================================

/**
 * The writing half of a message, as a handle: what a \c server::Response and a \c client::Request
 * have in common, and all that an operation which only produces a body -- \c send() -- needs to
 * see of either.
 *
 * The counterpart of \c Reader, and owned the same way: \c server::Response and \c client::Request
 * derive from it and add no data members of their own, so moving one into a \c Writer hands off
 * the writing half with its ownership intact. See \c Reader for why the destructor is not virtual.
 */
class Writer
{
public:
   /// The implementation, defined in \c writer_impl.hpp: what a protocol backend implements to
   /// be written to. Only code that implements or narrows one needs to see it.
   class Impl;

   using executor_type = Executor;

   explicit Writer(std::shared_ptr<Impl> impl) noexcept;
   Writer(Writer&&) noexcept;
   Writer& operator=(Writer&&) noexcept;
   ~Writer();

   /// Releases the implementation, as the destructor does. Writing afterwards fails.
   void reset() noexcept;

   explicit constexpr operator bool() const noexcept { return static_cast<bool>(impl_); }

   /// The executor of the session this message belongs to, or an empty one after \c reset().
   executor_type get_executor() const noexcept;

   /// The \c [proto:address:port.stream] tag the library's log lines carry for this message,
   /// without the brackets, or an empty string after \c reset().
   std::string log_prefix() const;

   /// Announces the length of the outgoing body, before its header is submitted.
   void content_length(std::optional<size_t> content_length);

#if ANYHTTP_ASIO
   /**
    * Writes \p buffer as part of the outgoing body, which stays open for more.
    *
    * An empty buffer writes nothing and completes immediately -- use \c async_write_eof() to end
    * the body.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_write(asio::const_buffer buffer, CompletionToken&& token = CompletionToken())
   {
      return asio::async_initiate<CompletionToken, Write>(
         asio::bind_executor(write_executor(token),
                             [this](WriteHandler handler, asio::const_buffer buffer) { //
                                async_write_any(std::move(handler), buffer, false);
                             }),
         token, buffer);
   }

   /**
    * Writes \p buffer as the last part of the outgoing body and ends it.
    *
    * Both go out together, so ending a body that has a tail of data left costs no more than
    * writing that tail: no second, empty write and no extra round trip through the protocol
    * stack. Re-ending an already-ended body with an empty buffer completes immediately and
    * changes nothing; with data attached it completes with \c errc::broken_pipe, just as writing
    * that data would -- there is no body left for it to belong to.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_write_eof(asio::const_buffer buffer, CompletionToken&& token = CompletionToken())
   {
      return asio::async_initiate<CompletionToken, Write>(
         asio::bind_executor(write_executor(token),
                             [this](WriteHandler handler, asio::const_buffer buffer) { //
                                async_write_any(std::move(handler), buffer, true);
                             }),
         token, buffer);
   }

   /// Ends the outgoing body without writing anything more.
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken = DefaultCompletionToken>
   auto async_write_eof(CompletionToken&& token = CompletionToken())
   {
      return async_write_eof(asio::const_buffer{}, std::forward<CompletionToken>(token));
   }
#endif

   //
   // The coroutine spelling of the operation(s) above, which both runtimes have: no completion
   // token, the result as a tuple, and errors reported, never thrown. Code that has to compile
   // with either runtime -- the library's own request handlers, the shared tests -- uses this.
   //

   /// Writes \p buffer as part of the body: <tt>auto [ec] = co_await writer.write(buffer);</tt>
   Task<std::tuple<error_code>> write(asio::const_buffer buffer);

   /// Writes \p buffer, if any, and ends the body: <tt>auto [ec] = co_await
   /// writer.write_eof();</tt>
   Task<std::tuple<error_code>> write_eof(asio::const_buffer buffer = {});

#if ANYHTTP_COROSIO
   /**
    * Writes the first non-empty buffer of \p buffers as part of the body, and yields its size:
    * this is what makes a Writer a \c capy::WriteStream. The body stays open, as with write().
    */
   template <capy::ConstBufferSequence Buffers>
   Task<std::tuple<error_code, size_t>> write_some(Buffers buffers)
   {
      for (auto it = capy::begin(buffers); it != capy::end(buffers); ++it)
         if (capy::const_buffer buffer = *it; buffer.size() > 0)
         {
            auto [ec] = co_await write(asio::const_buffer(buffer.data(), buffer.size()));
            co_return std::tuple{ec, ec ? size_t{0} : buffer.size()};
         }
      co_return std::tuple{error_code{}, size_t{0}};
   }
#endif

protected:
   /// The implementation, for the derived handle to narrow to its own \c Impl. Never null.
   Impl& pimpl() const noexcept { return *impl_; }

private:
#if ANYHTTP_ASIO
   //
   // Binding an executor to the initiating function lets tokens that need one -- the timer behind
   // cancel_after -- find it here, with the token's own executor taking precedence as usual. A
   // writer whose implementation is already gone (the handle was released while a write was still
   // outstanding, see the SpawnAndForget test) has none to offer, and the token is left with
   // whatever it brought itself. Such a write fails with bad_descriptor without touching an
   // executor at all, so an empty one here is never used.
   //
   template <typename CompletionToken>
   asio::associated_executor_t<CompletionToken, executor_type>
   write_executor(const CompletionToken& token) const noexcept
   {
      return asio::get_associated_executor(token, get_executor());
   }

   void async_write_any(WriteHandler&& handler, asio::const_buffer buffer, bool eof);
#endif

   std::shared_ptr<Impl> impl_;
};

#if ANYHTTP_COROSIO
static_assert(capy::WriteStream<Writer>);
#endif

// =================================================================================================

} // namespace anyhttp
