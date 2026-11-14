#pragma once

//
// The bytes a session starts with, and what an HTTP/2 session reads into: a dynamic buffer in one
// piece of memory, with the prepare()/commit()/consume() interface of Asio's DynamicBuffer_v1.
// Beast's flat_buffer did this before; Boost.HTTP keeps its input inside its parser and has no
// such thing to offer.
//

#include <boost/asio/buffer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

namespace anyhttp::detail
{

// =================================================================================================

class FlatBuffer
{
public:
   using const_buffers_type = boost::asio::const_buffer;
   using mutable_buffers_type = boost::asio::mutable_buffer;

   FlatBuffer() = default;
   FlatBuffer(FlatBuffer&& other) noexcept { *this = std::move(other); }
   FlatBuffer& operator=(FlatBuffer&& other) noexcept
   {
      storage_ = std::move(other.storage_);
      capacity_ = std::exchange(other.capacity_, 0);
      begin_ = std::exchange(other.begin_, 0);
      end_ = std::exchange(other.end_, 0);
      prepared_ = std::exchange(other.prepared_, 0);
      return *this;
   }

   /// The readable bytes.
   size_t size() const noexcept { return end_ - begin_; }

   /// How many bytes the buffer can hold without allocating.
   size_t capacity() const noexcept { return capacity_; }

   const_buffers_type data() const noexcept { return {storage_.get() + begin_, size()}; }
   mutable_buffers_type data() noexcept { return {storage_.get() + begin_, size()}; }

   /// Makes room for at least \p n bytes in all, without changing what is readable.
   void reserve(size_t n)
   {
      if (n > capacity_)
         grow(n);
   }

   /// Space for \p n more bytes behind the readable ones, valid until the next modifying call.
   mutable_buffers_type prepare(size_t n)
   {
      if (capacity_ - end_ < n)
      {
         if (capacity_ - size() >= n)
            compact();
         else
            grow(std::max(size() + n, 2 * capacity_));
      }
      prepared_ = n;
      return {storage_.get() + end_, n};
   }

   /// Makes \p n of the bytes last prepared readable.
   void commit(size_t n) noexcept
   {
      end_ += std::min(n, prepared_);
      prepared_ = 0;
   }

   /// Drops the first \p n readable bytes.
   void consume(size_t n) noexcept
   {
      begin_ += std::min(n, size());
      if (begin_ == end_)
         begin_ = end_ = 0;
   }

   void clear() noexcept { begin_ = end_ = prepared_ = 0; }

private:
   void compact() noexcept
   {
      if (begin_ == 0)
         return;
      if (size())
         std::memmove(storage_.get(), storage_.get() + begin_, size());
      end_ -= begin_;
      begin_ = 0;
   }

   void grow(size_t n)
   {
      auto storage = std::make_unique_for_overwrite<char[]>(n);
      if (size())
         std::memcpy(storage.get(), storage_.get() + begin_, size());
      end_ -= begin_;
      begin_ = 0;
      storage_ = std::move(storage);
      capacity_ = n;
   }

   std::unique_ptr<char[]> storage_;
   size_t capacity_ = 0;
   size_t begin_ = 0; ///< first readable byte
   size_t end_ = 0; ///< one past the last readable byte
   size_t prepared_ = 0; ///< what prepare() handed out last
};

// =================================================================================================

} // namespace anyhttp::detail
