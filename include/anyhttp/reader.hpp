#pragma once

#include "common.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>

#include <concepts>
#include <memory>
#include <optional>

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
   /**
    * What a protocol backend implements to be read from, and what the handles of both sides
    * narrow to their own: \c server::Request::Impl and \c client::Response::Impl derive from this
    * and add whatever else their side of the message has to offer.
    */
   class Impl : public std::enable_shared_from_this<Impl>
   {
   public:
      virtual ~Impl() = default;
      virtual asio::any_io_executor get_executor() const noexcept = 0;
      virtual std::optional<size_t> content_length() const noexcept = 0;

      //
      // Reads at most one buffer worth of the incoming body. The end of the body is reported the
      // way ASIO reports it everywhere else: \c asio::error::eof with zero bytes, and again for
      // every further read -- including reads issued after the underlying stream object is long
      // gone. A body that ends before it was supposed to -- a reset stream, a connection that went
      // away mid-message -- is reported as \c http::error::partial_message instead, so the two
      // cases stay distinguishable.
      //
      // An empty buffer is not a request to do anything; it completes immediately with success and
      // zero bytes, wherever the body stands.
      //
      virtual void async_read_some(asio::mutable_buffer buffer, ReadSomeHandler&& handler) = 0;
      virtual void detach() = 0;
      virtual void destroy() {};
   };

   // ----------------------------------------------------------------------------------------------

   using executor_type = asio::any_io_executor;

   Reader() noexcept = default;
   explicit Reader(std::shared_ptr<Impl> impl) noexcept;
   Reader(Reader&&) noexcept;
   Reader& operator=(Reader&&) noexcept;
   ~Reader();

   /// Releases the implementation, as the destructor does. Reading afterwards fails.
   void reset() noexcept;

   constexpr operator bool() const noexcept { return static_cast<bool>(m_impl); }

   /// The executor of the session this message belongs to, or an empty one after \c reset().
   executor_type get_executor() const noexcept;

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
      return asio::async_initiate<CompletionToken, ReadSome>(
         [&](ReadSomeHandler handler, asio::mutable_buffer buffer) { //
            async_read_some_any(buffer, std::move(handler));
         },
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
   Impl& pimpl() const noexcept { return *m_impl; }

private:
   void async_read_some_any(asio::mutable_buffer buffer, ReadSomeHandler&& handler);

   std::shared_ptr<Impl> m_impl;
};

// =================================================================================================

} // namespace anyhttp
