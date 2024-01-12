
#include "anyhttp/stream.hpp"
#include "anyhttp/session.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <nghttp2/nghttp2.h>

using namespace boost::asio::experimental::awaitable_operators;

namespace anyhttp::server
{
namespace nghttp2
{

// =================================================================================================

NGHttp2Request::NGHttp2Request(Stream& stream) : Impl(), stream(&stream) { stream.request = this; }

NGHttp2Request::~NGHttp2Request()
{
   if (stream)
      stream->request = nullptr;
}

void NGHttp2Request::detach() { stream = nullptr; }

// -------------------------------------------------------------------------------------------------

const asio::any_io_executor& NGHttp2Request::executor() const
{
   assert(stream);
   return stream->executor();
}

void NGHttp2Request::async_read_some(Request::ReadSomeHandler&& handler)
{
   assert(stream);
   assert(!stream->m_read_handler);
   stream->m_read_handler = std::move(handler);
   stream->call_handler_loop();
}

// =================================================================================================

NGHttp2Response::NGHttp2Response(Stream& stream) : stream(&stream) { stream.response = this; }

NGHttp2Response::~NGHttp2Response()
{
   if (stream)
      stream->response = nullptr;
}

void NGHttp2Response::detach() { stream = nullptr; }

// -------------------------------------------------------------------------------------------------

const asio::any_io_executor& NGHttp2Response::executor() const
{
   assert(stream);
   return stream->executor();
}

void NGHttp2Response::write_head(unsigned int status_code, Headers headers)
{
   assert(stream);

   auto nva = std::vector<nghttp2_nv>();
   nva.reserve(2);
   std::string date = "Sat, 01 Apr 2023 09:33:09 GMT";
   nva.push_back(make_nv_ls(":status", fmt::format("{}", status_code)));
   nva.push_back(make_nv_ls("date", date));

   // TODO: headers
   std::ignore = headers;

   prd.source.ptr = stream;
   prd.read_callback = [](nghttp2_session*, int32_t, uint8_t* buf, size_t length,
                          uint32_t* data_flags, nghttp2_data_source* source, void*) -> ssize_t
   {
      auto stream = static_cast<Stream*>(source->ptr);
      assert(stream);
      return stream->read_callback(buf, length, data_flags);
   };

   nghttp2_submit_response(stream->parent.session, stream->id, nva.data(), nva.size(), &prd);
}

void NGHttp2Response::async_write(Response::WriteHandler&& handler, std::vector<uint8_t> buffer)
{
   if (!stream)
      handler(boost::asio::error::basic_errors::connection_aborted);
   else
      stream->async_write(std::move(handler), std::move(buffer));
}

// =================================================================================================

} // namespace nghttp2
} // namespace anyhttp::server