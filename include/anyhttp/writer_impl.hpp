#pragma once

#include "common.hpp"
#include "writer.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>

#include <memory>
#include <optional>

namespace anyhttp
{

// =================================================================================================

/**
 * What a protocol backend implements to be written to, and what the handles of both sides narrow
 * to their own: \c server::Response::Impl and \c client::Request::Impl derive from this and add
 * whatever else their side of the message has to offer.
 */
class Writer::Impl : public std::enable_shared_from_this<Writer::Impl>
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

// =================================================================================================

} // namespace anyhttp
