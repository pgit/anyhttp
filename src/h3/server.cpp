//
// anyhttp HTTP/3 server.
//
// Nearly everything that makes HTTP/3 work is shared with the client and lives in
// anyhttp/h3/application.hpp and anyhttp/h3/stream.hpp, and everything about QUIC itself in
// src/quic/: `Http3ServerApplication` is the `http3::Http3Application` the QUIC server runs for
// connections that negotiate "h3", and `Http3ServerStream` is an `http3::Http3Stream` that reads a
// request and writes a response, where the client's does the opposite. Per-request state feeds an
// `Http3Reader` (server::Request) and `Http3Writer` (server::Response) into the same
// `RequestHandler` used by the HTTP/1.1 and HTTP/2 backends.
//

#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h3/application.hpp"
#include "anyhttp/h3/backend.hpp"
#include "anyhttp/h3/common.hpp"
#include "anyhttp/h3/stream.hpp"
#include "anyhttp/quic/session.hpp"
#include "anyhttp/request_handlers.hpp" // IWYU pragma: keep
#include "anyhttp/server_impl.hpp"

#include <boost/beast/http/status.hpp>
#include <boost/url/parse.hpp>

#include <nghttp3/nghttp3.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using anyhttp::http3::log_headers;
using anyhttp::http3::make_nv;

namespace anyhttp::server
{

// =================================================================================================
// Http3ServerApplication / Http3ServerStream: the server's end of the shared HTTP/3
// implementation.
// =================================================================================================

namespace
{

class Http3ServerApplication : public http3::Http3Application
{
public:
   Http3ServerApplication(quic::QuicSession& session, Server::Impl& server)
      : Http3Application(session, server.config().max_header_size), server_(server)
   {
   }

   std::shared_ptr<quic::QuicStream> make_stream(int64_t id) override;

   //
   // How many requests a client may have in progress is the QUIC limit on the streams it may open,
   // see server::Config::max_concurrent_streams. Only the server announces one: the streams a
   // client lets the server open carry nothing in HTTP/3, server push not being implemented.
   //
   void fill_transport_params(ngtcp2_transport_params& params) override
   {
      Http3Application::fill_transport_params(params);
      params.initial_max_streams_bidi = server_.config().max_concurrent_streams;
   }

   const RequestHandler& request_handler() const noexcept { return server_.request_handler(); }

private:
   Server::Impl& server_;
};

// -------------------------------------------------------------------------------------------------

class Http3ServerStream : public http3::Http3Stream
{
public:
   Http3ServerStream(Http3ServerApplication& application, int64_t id);

   //
   // Reads a request, writes a response -- the mirror image of Http3ClientStream. The response
   // body is handed to nghttp3 by reference (WriteMode::ZeroCopy): a file served through
   // serve_file() travels from the page cache into QUIC packets without an intermediate copy.
   //
   void on_pseudo_header(std::string_view name, std::string_view value) override;
   void on_headers_complete() override;
   void submit_response(unsigned int status_code, const Fields& fields) override;
};

// -------------------------------------------------------------------------------------------------

std::shared_ptr<quic::QuicStream> Http3ServerApplication::make_stream(int64_t id)
{
   return std::make_shared<Http3ServerStream>(*this, id);
}

} // namespace

// =================================================================================================
// Http3ServerStream implementation
// =================================================================================================

namespace
{

//
// The reading half of a server request: what http3::Http3Reader has for both roles, plus the
// request line, which only this role has. Its counterpart on the client is Http3ResponseReader.
// Like the fields, the request line is moved out of the stream on detach(): a handler may well
// look at it only after the session is gone, e.g. when it first runs during shutdown.
//
class Http3RequestReader final : public http3::Http3Reader<server::Request::Impl>
{
public:
   using Http3Reader<server::Request::Impl>::Http3Reader;

   std::string_view method() const noexcept override
   {
      return stream ? stream->method : detached_method;
   }

   boost::url_view url() const override { return stream ? stream->url : detached_url; }

   void detach() override
   {
      assert(stream);
      detached_method = std::move(stream->method);
      detached_url = std::move(stream->url);
      Http3Reader::detach();
   }

private:
   std::string detached_method;
   boost::urls::url detached_url;
};

} // namespace

// -------------------------------------------------------------------------------------------------

Http3ServerStream::Http3ServerStream(Http3ServerApplication& app, int64_t stream_id)
   : http3::Http3Stream(app, stream_id, quic::WriteMode::ZeroCopy)
{
}

void Http3ServerStream::on_pseudo_header(std::string_view name, std::string_view value)
{
   if (name == ":method")
      method = value;
   else if (name == ":path")
   {
      if (auto parsed = boost::urls::parse_relative_ref(value); parsed.has_value())
      {
         url.set_path(parsed->path());
         if (parsed->has_query())
            url.set_query(parsed->query());
         if (parsed->has_fragment())
            url.set_fragment(parsed->fragment());
      }
   }
   else if (name == ":scheme")
      url.set_scheme(value);
   else if (name == ":authority")
      url.set_encoded_authority(value);
}

void Http3ServerStream::on_headers_complete()
{
   mlogd("{} {}", method, url.buffer());
   log_headers(log_prefix_, std::exchange(received_headers, {}));

   //
   // Build the user-facing Request/Response and dispatch through the shared handler.
   //
   server::Request request(std::make_unique<Http3RequestReader>(*this));
   server::Response response(std::make_unique<http3::Http3Writer<server::Response::Impl>>(*this));

   auto& app = static_cast<Http3ServerApplication&>(application);
   if (header_limit_exceeded)
      launch(get_executor(), header_fields_too_large(std::move(request), std::move(response)));
   else if (auto& handler = app.request_handler())
      launch(get_executor(), handler(std::move(request), std::move(response)));
   else
   {
      mloge("no request handler set");
      launch(get_executor(), not_found(std::move(response)));
   }
}

void Http3ServerStream::submit_response(unsigned int status, const Fields& user_fields)
{
   assert(!headers_submitted);

   response_status = status;
   response_fields = user_fields;

   auto status_str = std::to_string(response_status);
   std::vector<nghttp3_nv> nva;
   nva.reserve(16); // small typical header count; vector will grow if needed
   const auto date = http_date_now();
   nva.push_back(make_nv(":status", status_str));
   nva.push_back(make_nv("date", date));
   nva.push_back(make_nv("server", "anyhttp-quic/0.1"));

   if (response_content_length)
   {
      response_content_length_str = std::to_string(*response_content_length);
      nva.push_back(make_nv("content-length", response_content_length_str));
   }

   for (auto&& item : response_fields)
   {
      if (item.name_string().starts_with(':'))
      {
         mlogw("submit_response: dropping pseudo-header '{}'", item.name_string());
         continue;
      }
      nva.push_back(make_nv(item.name_string(), item.value()));
   }

   using namespace boost::beast::http;
   mlogd("{} {}", response_status, obsolete_reason(int_to_status(response_status)));

   if (submit_headers(nva, false /* response */))
      session.wake_write();
}

// =================================================================================================

quic::ApplicationFactory make_http3_application(Server::Impl& server)
{
   return [&server](quic::QuicSession& session) -> std::unique_ptr<quic::QuicApplication> {
      return std::make_unique<Http3ServerApplication>(session, server);
   };
}

// =================================================================================================

} // namespace anyhttp::server
