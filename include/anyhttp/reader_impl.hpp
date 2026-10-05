#pragma once

#include "common.hpp"
#include "reader.hpp"

#include <boost/asio/buffer.hpp>

#include <memory>
#include <optional>
#include <string>

namespace anyhttp
{

// =================================================================================================

/**
 * What a protocol backend implements to be read from, and what the handles of both sides narrow
 * to their own: \c server::Request::Impl and \c client::Response::Impl derive from this and add
 * whatever else their side of the message has to offer.
 */
class Reader::Impl : public std::enable_shared_from_this<Reader::Impl>
{
public:
   virtual ~Impl() = default;
   virtual Executor get_executor() const noexcept = 0;

   /// The log prefix of the stream this belongs to, still valid after detach().
   virtual std::string log_prefix() const = 0;
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

   //
   // Every operation comes in two shapes: completing a handler (async_*()), which the completion
   // token API is built on, and as a coroutine, which the coroutine spelling of the API awaits. A
   // backend implements the shape that suits it, and gets the other one from it. Parking the
   // handler (HTTP/2, HTTP/3) makes the coroutine cheap, through initiate(). Doing the I/O in a
   // coroutine (HTTP/1.1) makes the handler shape cost a spawn, through launch().
   //
   virtual Task<std::tuple<error_code, size_t>> read_some(asio::mutable_buffer buffer)
   {
      co_return co_await initiate<ReadSome>([this, buffer](ReadSomeHandler handler) { //
         async_read_some(buffer, std::move(handler));
      });
   }

   virtual void detach() = 0;

   /// Called by the implementation from its destructor.
   virtual void destroy() {};
};

// =================================================================================================

} // namespace anyhttp
