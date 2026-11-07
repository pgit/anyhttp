#pragma once

#include "common.hpp"

#include <boost/asio/buffer.hpp>

#if ANYHTTP_COROSIO
#include <boost/capy/concept/mutable_buffer_sequence.hpp>
#include <boost/capy/concept/read_stream.hpp>
#else
#include "anyhttp/asio/concepts.hpp"

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#endif

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
   /// \cond internal
   /// The implementation, defined in \c reader_impl.hpp: what a protocol backend implements to
   /// be read from. Only code that implements or narrows one needs to see it.
   class Impl;

   explicit Reader(std::shared_ptr<Impl> impl) noexcept;
   Reader(Reader&&) noexcept;
   Reader& operator=(Reader&&) noexcept;
   ~Reader();
   /// \endcond

   /// The executor type, for what looks for one.
   using executor_type = Executor;

   /// Releases the implementation, as the destructor does. Reading afterwards fails.
   void reset() noexcept;

   /// \return Whether this handle has an implementation: not after \c reset(), or being moved from.
   explicit constexpr operator bool() const noexcept { return static_cast<bool>(impl_); }

   /**
    * \return The executor of the session this message belongs to, or an empty one after
    * \c reset().
    *
    * Without one, what is started on an empty handle with a completion token cannot complete the
    * way ASIO promises: it fails with \c errors::bad_descriptor from inside the call, and a token
    * that needs an executor of the operation, as \c cancel_after does for its timer, throws
    * \c bad_executor instead. Awaited, it just fails.
    */
   executor_type get_executor() const noexcept;

   /// \return The <tt>[proto:address:port.stream]</tt> tag the library's log lines carry for this
   /// message, without the brackets, or an empty string after \c reset().
   std::string log_prefix() const;

   /// \return What the incoming message announced as its body length, if it announced one.
   std::optional<size_t> content_length() const noexcept;

#if ANYHTTP_ASIO
   /**
    * Reads a part of the incoming body.
    *
    * The end of the body is reported as \c errors::eof with zero bytes, as the runtime does
    * everywhere else, and so is every read after it. A body cut short by a reset stream or a lost
    * connection completes with \c errors::partial_message instead. See the page on the end of
    * a body.
    *
    * \param buffer Where to put what has been read, which has to stay valid until the operation
    *               completes.
    * \param token The completion token, for a handler of the signature \c ReadSome.
    * \return What \p token makes of the operation.
    */
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadSome) CompletionToken = DefaultCompletionToken>
   auto async_read_some(boost::asio::mutable_buffer buffer,
                        CompletionToken&& token = CompletionToken())
   {
      //
      // Binding an executor to the initiating function lets tokens that need one -- the timer
      // behind cancel_after -- find it here, see Writer.
      //
      return boost::asio::async_initiate<CompletionToken, ReadSome>(
         boost::asio::bind_executor(
            boost::asio::get_associated_executor(token, get_executor()),
            [this](ReadSomeHandler handler, boost::asio::mutable_buffer buffer) { //
               async_read_some_any(buffer, std::move(handler));
            }),
         token, buffer);
   }

   /**
    * \overload
    *
    * When given an actual sequence of buffers, this fills only the first non-empty one.
    *
    * \param buffers Where to put what has been read, which has to stay valid until the operation
    *                completes.
    * \param token The completion token, for a handler of the signature \c ReadSome.
    * \return What \p token makes of the operation.
    */
   template <typename Buffers,
             BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadSome) CompletionToken = DefaultCompletionToken>
      requires(boost::asio::is_mutable_buffer_sequence<Buffers>::value &&
               !std::convertible_to<const Buffers&, boost::asio::mutable_buffer>)
   auto async_read_some(const Buffers& buffers, CompletionToken&& token = CompletionToken())
   {
      for (auto& buffer : buffers)
         if (buffer.size() > 0)
            return async_read_some(boost::asio::mutable_buffer(buffer),
                                   std::forward<CompletionToken>(token));

      return async_read_some(boost::asio::mutable_buffer{}, std::forward<CompletionToken>(token));
   }
#endif

   //
   // The coroutine spelling of the operation(s) above, which both runtimes have: no completion
   // token, the result as a tuple, and errors reported, never thrown. Code that has to compile
   // with either runtime -- the library's own request handlers, the shared tests -- uses this.
   //

   /// Reads a part of the incoming body: <tt>auto [ec, n] = co_await reader.read_some(buffer);</tt>
   /// \param buffer Where to put what has been read.
   /// \return The error, and how many bytes have been read.
   IoTask<size_t> read_some(boost::asio::mutable_buffer buffer);

   /// \overload
   ///
   /// As with async_read_some(), this fills only the first non-empty buffer.
   ///
   /// \param buffers Where to put what has been read.
   /// \return The error, and how many bytes have been read.
   template <typename Buffers>
      requires(boost::asio::is_mutable_buffer_sequence<Buffers>::value &&
               !std::convertible_to<const Buffers&, boost::asio::mutable_buffer>)
   IoTask<size_t> read_some(const Buffers& buffers)
   {
      for (auto& buffer : buffers)
         if (buffer.size() > 0)
            return read_some(boost::asio::mutable_buffer(buffer));
      return read_some(boost::asio::mutable_buffer{});
   }

#if ANYHTTP_COROSIO
   /// \overload
   ///
   /// For capy's buffer sequences, which is what makes a Reader a \c capy::ReadStream.
   template <capy::MutableBufferSequence Buffers>
   IoTask<size_t> read_some(Buffers buffers)
   {
      for (auto it = capy::buffer_sequence_begin(buffers); it != capy::buffer_sequence_end(buffers);
           ++it)
         if (capy::mutable_buffer buffer = *it; buffer.size() > 0)
            return read_some(boost::asio::mutable_buffer(buffer.data(), buffer.size()));
      return read_some(boost::asio::mutable_buffer{});
   }
#endif

protected:
   /// \cond internal
   /// The implementation, for the derived handle to narrow to its own \c Impl. Never null.
   Impl& pimpl() const noexcept { return *impl_; }
   /// \endcond

private:
#if ANYHTTP_ASIO
   void async_read_some_any(boost::asio::mutable_buffer buffer, ReadSomeHandler&& handler);
#endif

   std::shared_ptr<Impl> impl_;
};

#if ANYHTTP_COROSIO
static_assert(capy::ReadStream<Reader>);
#else
static_assert(AsyncReadStream<Reader>);
#endif

// =================================================================================================

} // namespace anyhttp
