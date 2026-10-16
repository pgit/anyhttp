//
// QuicStream: one QUIC stream, shared by the server and the client and by whatever protocol runs
// on top. See anyhttp/quic/stream.hpp.
//
#include "anyhttp/quic/stream.hpp"
#include "anyhttp/quic/session.hpp"

#include <format>

namespace anyhttp::quic
{

// =================================================================================================

QuicStream::QuicStream(QuicSession& s, int64_t stream_id) : id(stream_id), session(s)
{
   log_prefix_ = std::format("{}.{}", session.log_prefix(), id);
}

Executor QuicStream::get_executor() const noexcept { return session.get_executor(); }

// =================================================================================================

} // namespace anyhttp::quic
