#pragma once

#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <concepts>

namespace anyhttp
{

template <typename T>
concept AsyncStream =
   requires(T stream, boost::asio::mutable_buffer buffer, boost::asio::const_buffer const_buffer,
            boost::system::error_code ec,
            std::function<void(boost::system::error_code, std::size_t)> handler) {
      // async_read_some
      { stream.async_read_some(buffer, handler) } -> std::same_as<void>;

      // async_write_some
      { stream.async_write_some(const_buffer, handler) } -> std::same_as<void>;
   };

template <typename T, typename MutableBufferSequence, typename Handler>
concept AsyncReadStream = requires(T t, const MutableBufferSequence& buffers, Handler&& handler) {
   { t.async_read_some(buffers, std::forward<Handler>(handler)) };
   { t.get_executor() };
   requires std::is_destructible_v<T>;
   requires std::is_move_constructible_v<T>;
};

} // namespace anyhttp