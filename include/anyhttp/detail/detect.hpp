#pragma once

//
// Telling the protocol a client speaks on a new TCP connection from the first bytes it sends:
// TLS (and then ALPN decides), the HTTP/2 client preface (h2c with prior knowledge), or anything
// else, which is taken for HTTP/1.1.
//

#include "anyhttp/runtime.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/logic/tribool.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <tuple>

namespace anyhttp::server::detail
{

// =================================================================================================

/// Whether \p buffers start with \p prefix: yes, no, or not enough of them yet to tell.
template <typename ConstBufferSequence>
boost::tribool buffer_sequence_starts_with(const ConstBufferSequence& buffers,
                                           std::string_view prefix)
{
   std::size_t matched = 0;
   const auto end = asio::buffer_sequence_end(buffers);
   for (auto it = asio::buffer_sequence_begin(buffers); it != end && matched < prefix.size(); ++it)
   {
      const asio::const_buffer buffer(*it);
      const auto to_compare = std::min(buffer.size(), prefix.size() - matched);
      if (std::memcmp(buffer.data(), prefix.data() + matched, to_compare) != 0)
         return false;
      matched += to_compare;
   }
   return matched == prefix.size() ? boost::tribool(true) : boost::indeterminate;
}

enum class Detected
{
   tls,
   h2c,
   h1
};

/**
 * Tells what the client on \p socket speaks.
 *
 * TLS is told from the first byte, a handshake record (0x16), which neither an HTTP/1.1 request
 * nor the HTTP/2 client preface can start with. That byte is only peeked at, so the TLS stream
 * finds the whole ClientHello on the socket itself and needs no bytes handed over.
 *
 * Telling the HTTP/2 client preface from HTTP/1.1 takes up to 24 bytes, and these are read into
 * \p buffer: the session that takes over the connection starts with what is in there.
 */
template <typename Socket, typename DynamicBuffer>
Task<std::tuple<error_code, Detected>> detect(Socket& socket, DynamicBuffer& buffer)
{
   std::array<std::uint8_t, 1> first;
   auto [ec, n] = co_await io::peek(socket, asio::buffer(first));
   if (ec)
      co_return std::tuple{ec, Detected::h1};
   if (n == 1 && first[0] == 0x16)
      co_return std::tuple{error_code{}, Detected::tls};

   for (;;)
   {
      auto preface = buffer_sequence_starts_with(buffer.data(), "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
      if (!boost::indeterminate(preface))
         co_return std::tuple{error_code{}, preface ? Detected::h2c : Detected::h1};

      auto [read_ec, read] = co_await io::read_some(socket, buffer.prepare(1460));
      if (read_ec)
         co_return std::tuple{read_ec, Detected::h1};
      buffer.commit(read);
   }
}

// =================================================================================================

} // namespace anyhttp::server::detail
