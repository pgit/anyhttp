//
// https://github.com/boostorg/beast/issues/3032
// https://godbolt.org/z/WaonxPoYr
//
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>

#include <iostream>

using namespace std::string_view_literals;

namespace asio = boost::asio;
namespace http = boost::beast::http;
using error_code = boost::system::error_code;

/// Dummy stream satisfying the AsyncWriteStream requirements
struct StringWriteStream
{
   template <typename ConstBufferSequence>
      requires boost::beast::is_const_buffer_sequence<ConstBufferSequence>::value
   size_t write_some(const ConstBufferSequence& buffers, error_code&)
   {
      size_t written = 0;
      for (auto it : buffers)
      {
         buffer += std::string_view(static_cast<const char*>(it.data()), it.size());
         written += it.size();
      }
      return written;
   }

   template <typename ConstBufferSequence>
      requires boost::beast::is_const_buffer_sequence<ConstBufferSequence>::value
   size_t write_some(const ConstBufferSequence& buffers)
   {
      error_code ec;
      auto written = write_some(buffers, ec);
      if (ec)
         BOOST_THROW_EXCEPTION(boost::system::system_error{ec});
   };

   std::string buffer;
};

static_assert(boost::beast::is_sync_write_stream<StringWriteStream>::value);

void test(bool with_nullptr)
{
   StringWriteStream stream;

   using Serializer = http::response_serializer<http::buffer_body>;
   using Message = std::remove_const_t<typename Serializer::value_type>;
   Message message;
   Serializer serializer{message};

   message.chunked(true);

   error_code ec;
   auto text = asio::buffer("Hello, World!"sv);
   message.body().data = const_cast<void*>(text.data());
   message.body().size = text.size();
   message.body().more = true;
   http::write(stream, serializer, ec);
   assert(ec == http::error::need_buffer);

   if (with_nullptr)
      message.body().data = nullptr;
   message.body().size = 0;
   message.body().more = false;
   http::write(stream, serializer, ec);
   assert(!ec);

   std::cout << stream.buffer;
}

int main()
{
   std::cout << "--- GOOD -----------------" << std::endl;
   test(true);
   std::cout << "--- BAD ------------------" << std::endl;
   test(false);
   std::cout << "--------------------------" << std::endl;
}
