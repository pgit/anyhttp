#pragma once

#include "common.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>

#include <memory>
#include <optional>

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
   /**
    * What a protocol backend implements to be written to, and what the handles of both sides
    * narrow to their own: \c server::Response::Impl and \c client::Request::Impl derive from this
    * and add whatever else their side of the message has to offer.
    */
   class Impl : public std::enable_shared_from_this<Impl>
   {
   public:
      virtual ~Impl() = default;
      virtual asio::any_io_executor get_executor() const noexcept = 0;
      virtual void content_length(std::optional<size_t> content_length) = 0;

      //
      // Writes \p buffer and, if \p eof is set, ends the outgoing body after it. The two travel
      // together on purpose: every backend can put the last bytes of a body and the flag that ends
      // it into the same protocol element -- one DATA frame with END_STREAM (HTTP/2), one QUIC
      // STREAM frame with FIN (HTTP/3), one last chunk (HTTP/1.1) -- so a message that ends with
      // data needs no second, empty write to close it out.
      //
      // Every implementation answers the same entry ladder, in this order: an empty buffer with
      // \p eof clear writes nothing at all and completes immediately with success, wherever the
      // body stands -- it is not, as it once was, how a body is ended. Once the body has been
      // ended, writing data -- through either entry point -- completes with \c errc::broken_pipe,
      // while re-ending it with no data attached is an idempotent no-op. Only then do stream-level
      // failures (closed, cancelled) get their say.
      //
      virtual void async_write(WriteHandler&& handler, asio::const_buffer buffer, bool eof) = 0;
      virtual void detach() = 0;
      virtual void destroy() {};
   };

   // ----------------------------------------------------------------------------------------------

   using executor_type = asio::any_io_executor;

   Writer() noexcept = default;
   explicit Writer(std::shared_ptr<Impl> impl) noexcept;
   Writer(Writer&&) noexcept;
   Writer& operator=(Writer&&) noexcept;
   ~Writer();

   /// Releases the implementation, as the destructor does. Writing afterwards fails.
   void reset() noexcept;

   constexpr operator bool() const noexcept { return static_cast<bool>(m_impl); }

   /// The executor of the session this message belongs to, or an empty one after \c reset().
   executor_type get_executor() const noexcept;

   /// Announces the length of the outgoing body, before its header is submitted.
   void content_length(std::optional<size_t> content_length);

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

protected:
   /// The implementation, for the derived handle to narrow to its own \c Impl. Never null.
   Impl& pimpl() const noexcept { return *m_impl; }

private:
   //
   // Binding an executor to the initiating function lets tokens that need one -- the timer behind
   // cancel_after -- find it here, with the token's own executor taking precedence as usual. A
   // writer whose implementation is already gone (the handle was released while a write was still
   // outstanding, see the SpawnAndForget test) has none to offer, and the token is left with
   // whatever it brought itself. Such a write fails with bad_descriptor without touching an
   // executor at all, so an empty one here is never used.
   //
   template <typename CompletionToken>
   auto write_executor(const CompletionToken& token) const noexcept
   {
      return asio::get_associated_executor(token, get_executor());
   }

   void async_write_any(WriteHandler&& handler, asio::const_buffer buffer, bool eof);

   std::shared_ptr<Impl> m_impl;
};

// =================================================================================================

} // namespace anyhttp
