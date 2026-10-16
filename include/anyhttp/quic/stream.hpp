#pragma once

#include "anyhttp/common.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace anyhttp::quic
{

class QuicSession;

// =================================================================================================

//
// One QUIC stream, as the session keeps track of it. What it carries, and how, is up to the
// application on top (see QuicApplication::make_stream()); the session only needs to be able to
// find a stream by its ID and to fail it when the connection goes away.
//
class QuicStream : public std::enable_shared_from_this<QuicStream>
{
public:
   QuicStream(QuicSession& session, int64_t id);
   virtual ~QuicStream() = default;

   QuicStream(const QuicStream&) = delete;
   QuicStream& operator=(const QuicStream&) = delete;

   int64_t id;
   QuicSession& session;
   std::string log_prefix_;

   Executor get_executor() const noexcept;
   const std::string& log_prefix() const noexcept { return log_prefix_; }

   //
   // The stream is over before it completed: it was reset, or the connection is going away. Fails
   // whatever is still pending on it.
   //
   virtual void fail(error_code ec) = 0;
};

// =================================================================================================

} // namespace anyhttp::quic
