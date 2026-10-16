#pragma once

#include "anyhttp/common.hpp"
#include "anyhttp/quic/stream.hpp"
#include "anyhttp/reader_impl.hpp"
#include "anyhttp/writer_impl.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/url/url.hpp>
#include <boost/url/urls.hpp>

#include <nghttp3/nghttp3.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace anyhttp::http3
{

class Http3Application;

// =================================================================================================

//
// One HTTP/3 request/response exchange: a QUIC stream (which has the body, both ways) with the
// header sections on top, and nghttp3 between it and ngtcp2. Both roles use this same class; what
// differs is only which half of the exchange travels in which direction -- the server reads a
// request and writes a response, the client writes a request and reads a response -- so
// everything below is phrased as the *incoming* and the *outgoing* message, and the handful of
// genuinely role-specific steps (which pseudo-headers to parse, what to do once the incoming
// headers are complete, how the outgoing headers are submitted) are virtual hooks implemented by
// Http3ServerStream / Http3ClientStream.
//
class Http3Stream : public quic::QuicStream
{
public:
   Http3Stream(Http3Application& application, int64_t id, quic::WriteMode write_mode);
   ~Http3Stream() override;

   Http3Application& application;

   //
   // Incoming message (request on the server, response on the client), populated by the nghttp3
   // header callbacks. Only one of method/status_code is ever meaningful, depending on the role.
   //
   std::string method;
   boost::urls::url url;
   unsigned int status_code = 0;
   std::optional<size_t> content_length;
   Fields fields;

   //
   // The header block as it arrived, buffered so that end_headers can log it in one go, below the
   // request/status line, instead of one stray line per header as they come in. Only filled when
   // debug logging is on, and dropped again as soon as it has been logged.
   //
   std::vector<std::pair<std::string, std::string>> received_headers;
   bool headers_received = false;

   /// Size of the received header section so far, see Config::max_header_size.
   size_t header_size = 0;
   bool header_limit_exceeded = false;

   //
   // Outgoing message: the response on the server (set by the user through Http3Writer), the
   // request on the client (set once by async_submit(), before this stream even exists as far as
   // the user is concerned).
   //
   unsigned int response_status = 0;
   Fields response_fields;
   std::optional<size_t> response_content_length;
   std::string response_content_length_str; // storage backing a nghttp3_nv
   bool headers_submitted = false;

   //
   // Outgoing body, as nghttp3 sees it. It takes each piece of data once and keeps it, and would
   // take a repeated offer as *additional*, distinct stream bytes -- duplicating the body on the
   // wire -- so data_reader() offers from tx_offered on, and a repeat call gets
   // NGHTTP3_ERR_WOULDBLOCK. What it reports back is counted in body bytes for acknowledgements,
   // but including its framing for what went into packets, see on_write_offered().
   //
   nghttp3_ssize data_reader(nghttp3_vec* vec, size_t veccnt, uint32_t* pflags);
   void on_write_acked(size_t n); // nghttp3 acked_stream_data
   void on_write_offered(size_t n); // bytes ngtcp2 committed to a packet

   //
   // Headers.
   //
   void on_header(std::string_view name, std::string_view value);
   void on_end_headers();

   /// Hands an assembled header block to nghttp3, as a request (client) or response (server).
   bool submit_headers(std::span<const nghttp3_nv> nva, bool is_request);

   void delete_writer() override;

protected:
   //
   // QuicStream
   //
   void on_writable() override;
   void reset_writing() override;
   uint64_t reset_error_code() const noexcept override { return NGHTTP3_H3_REQUEST_CANCELLED; }
   uint64_t stop_sending_error_code() const noexcept override { return NGHTTP3_H3_NO_ERROR; }

   //
   // Role-specific pieces. Everything else above is shared verbatim between server and client.
   //
   /// Parse a `:`-prefixed pseudo-header of the incoming message.
   virtual void on_pseudo_header(std::string_view name, std::string_view value) = 0;
   /// The incoming header block is complete: dispatch the request (server) / response (client).
   virtual void on_headers_complete() = 0;

public:
   /// Submit the outgoing response headers. A no-op on the client, whose request headers went out
   /// with the stream itself, see Http3ClientApplication::async_submit().
   virtual void submit_response(unsigned int status_code, const Fields& fields) = 0;
};

// =================================================================================================
// Http3Reader / Http3Writer: adapters plugging an Http3Stream into the anyhttp Reader/Writer
// interfaces. The same pair serves both roles -- server::Request/server::Response and
// client::Response/client::Request are the same two halves seen from the other end. What they add
// to the QUIC stream's adapters is the header section.
// =================================================================================================

template <typename Interface>
class Http3Reader : public quic::QuicReader<Interface, Http3Stream>
{
public:
   using quic::QuicReader<Interface, Http3Stream>::QuicReader;
   using quic::QuicReader<Interface, Http3Stream>::stream;

   std::optional<size_t> content_length() const noexcept override
   {
      return stream ? stream->content_length : std::nullopt;
   }

   const Fields& fields() const override { return stream ? stream->fields : detached_fields; }

   void detach() override
   {
      assert(stream);
      detached_fields = std::move(stream->fields); // the stream is being destroyed
      quic::QuicReader<Interface, Http3Stream>::detach();
   }

   Fields detached_fields; // moved out of the stream by detach(), so fields() outlives it
};

// -------------------------------------------------------------------------------------------------

template <typename Base>
class Http3Writer : public quic::QuicWriter<Base, Http3Stream>
{
public:
   using quic::QuicWriter<Base, Http3Stream>::QuicWriter;
   using quic::QuicWriter<Base, Http3Stream>::stream;

   void content_length(std::optional<size_t> len) override
   {
      assert(stream);
      stream->response_content_length = len;
   }

   void async_submit(StatusHandler&& handler, unsigned int status_code, const Fields& fields)
   {
      if (!stream || stream->closed)
      {
         std::move(handler)(make_error_code(errc::connection_reset));
         return;
      }
      stream->submit_response(status_code, fields);
      std::move(handler)(error_code{});
   }
};

// =================================================================================================

} // namespace anyhttp::http3
