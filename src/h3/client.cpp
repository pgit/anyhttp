//
// anyhttp HTTP/3 client.
//
// Almost all of it is shared with the server: `Http3ClientApplication` is an
// `http3::Http3Application` (see anyhttp/h3/application.hpp) on a QUIC connection the client end
// of src/quic/ makes, and `Http3ClientStream` is an `http3::Http3Stream` (anyhttp/h3/stream.hpp)
// that writes a request and reads a response, where the server's does the opposite.
//
// What is genuinely client-side here: async_submit(), which opens a stream and puts the request
// headers on it before handing a client::Request back to the caller.
//

#include "anyhttp/quic/client.hpp"
#include "anyhttp/client_impl.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h3/application.hpp"
#include "anyhttp/h3/backend.hpp"
#include "anyhttp/h3/common.hpp"
#include "anyhttp/h3/stream.hpp"
#include "anyhttp/quic/session.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/beast/http/error.hpp>
#include <boost/beast/http/status.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/detail/error_code.hpp>

#include <boost/url/url.hpp>

#include <boost/container/small_vector.hpp>

#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>

#include <charconv>
#include <memory>
#include <string>
#include <string_view>

using namespace std::chrono_literals;

using anyhttp::http3::log_headers;
using anyhttp::http3::make_nv;

namespace anyhttp::client
{

// =================================================================================================
// Http3ClientApplication / Http3ClientStream: the client's end of the shared HTTP/3
// implementation.
// =================================================================================================

namespace
{

class Http3ClientApplication : public http3::Http3Application
{
public:
   using Http3Application::Http3Application;

   std::shared_ptr<quic::QuicStream> make_stream(int64_t id) override;

   //
   // Opens a stream and puts the request headers on it before handing a client::Request back to
   // the caller, see QuicApplication::async_submit().
   //
   void async_submit(SubmitHandler&& handler, std::string_view method, boost::urls::url url,
                     const Fields& headers) override;
};

} // namespace

// -------------------------------------------------------------------------------------------------

namespace
{

class Http3ClientStream : public http3::Http3Stream
{
public:
   Http3ClientStream(Http3ClientApplication& application, int64_t id);
   ~Http3ClientStream() override;

   //
   // Writes a request, reads a response -- the mirror image of Http3ServerStream. The request
   // body goes through the staging buffer (WriteMode::Staged) rather than being handed to nghttp3
   // by reference: that keeps cancellation instantaneous and, more importantly, leaves the stream
   // intact afterwards, so a cancelled write can be followed by another one on the same request.
   //
   void on_pseudo_header(std::string_view name, std::string_view value) override;
   void on_headers_complete() override;
   void on_failed(error_code ec) override;

   /// The request headers went out with the stream itself, see
   /// Http3ClientApplication::async_submit().
   void submit_response(unsigned int, const Fields&) override {}

   /// Assembles and submits the request headers. Called once, right after the stream is created.
   bool submit_request(std::string_view method, const boost::urls::url& url, const Fields& headers);

   void async_get_response(client::Request::GetResponseHandler&& handler);
   void deliver_response();
   void deliver_failure();

   bool response_delivered = false;
   client::Request::GetResponseHandler response_handler;

   //
   // Why the stream died, remembered because async_get_response() may well be called only
   // afterwards -- a response that can never arrive must not leave its caller waiting forever.
   //
   error_code failure_ec;
};

// -------------------------------------------------------------------------------------------------

//
// The client's Request needs one thing the shared writer does not have: async_get_response(),
// which client.hpp exposes and server::Response has no counterpart for.
//
class Http3ClientWriter : public http3::Http3Writer<client::Request::Impl>
{
public:
   using http3::Http3Writer<client::Request::Impl>::Http3Writer;

   void async_get_response(client::Request::GetResponseHandler&& handler) override
   {
      if (!stream)
      {
         std::move(handler)(make_error_code(errc::connection_aborted), client::Response{nullptr});
         return;
      }
      static_cast<Http3ClientStream*>(stream)->async_get_response(std::move(handler));
   }
};

} // namespace

// =================================================================================================
// Http3ClientStream implementation
// =================================================================================================

namespace
{

//
// The reading half of a client response: what http3::Http3Reader has for both roles, plus the
// status code, which only this role has. Its counterpart on the server is Http3RequestReader.
//
class Http3ResponseReader final : public http3::Http3Reader<client::Response::Impl>
{
public:
   using Http3Reader<client::Response::Impl>::Http3Reader;

   unsigned int status_code() const noexcept override
   {
      return stream ? stream->status_code : detached_status_code;
   }

   void detach() override
   {
      assert(stream);
      detached_status_code = stream->status_code;
      Http3Reader::detach();
   }

private:
   unsigned int detached_status_code = 0;
};

} // namespace

// -------------------------------------------------------------------------------------------------

// -------------------------------------------------------------------------------------------------

Http3ClientStream::Http3ClientStream(Http3ClientApplication& app, int64_t stream_id)
   : http3::Http3Stream(app, stream_id, quic::WriteMode::Staged)
{
}

Http3ClientStream::~Http3ClientStream()
{
   if (!response_delivered && response_handler)
      swap_and_invoke(response_handler, make_error_code(errc::connection_reset),
                      client::Response{nullptr});
}

void Http3ClientStream::on_pseudo_header(std::string_view name, std::string_view value)
{
   if (name == ":status")
   {
      unsigned int status = 0;
      if (std::from_chars(value.begin(), value.end(), status).ec == std::errc{})
         status_code = status;
   }
}

void Http3ClientStream::on_headers_complete()
{
   //
   // A response with too large a header section is of no use: fail it, and stop the server from
   // sending its body.
   //
   if (header_limit_exceeded)
   {
      headers_received = false; // there is no response to deliver, see deliver_failure()
      failure_ec = errors::header_limit;
      session.reset_stream(id, NGHTTP3_H3_REQUEST_CANCELLED);
      deliver_failure();
      return;
   }

   using namespace boost::beast::http;
   mlogd("{} {}", status_code, obsolete_reason(int_to_status(status_code)));
   log_headers(log_prefix_, std::exchange(received_headers, {}));
   deliver_response();
}

void Http3ClientStream::on_failed(error_code ec)
{
   //
   // A stream closing gracefully (ec success, e.g. NGHTTP3_H3_NO_ERROR) still means no response
   // ever arrived if headers were never received -- never report success with a null Response.
   //
   if (!failure_ec) // the first reason is the one to report, see on_headers_complete()
      failure_ec =
         ec ? ec : error_code(boost::system::error_code(boost::beast::http::error::end_of_stream));
   deliver_failure();
}

void Http3ClientStream::deliver_failure()
{
   if (headers_received || response_delivered || !response_handler)
      return;

   response_delivered = true;
   swap_and_invoke(response_handler, failure_ec, client::Response{nullptr});
}

bool Http3ClientStream::submit_request(std::string_view method, const boost::urls::url& request_url,
                                       const Fields& headers)
{
   url = request_url;

   //
   // TODO: CONNECT
   //
   std::string method_str(method);
   std::string scheme(request_url.scheme());
   std::string target(request_url.encoded_target());
   std::string authority(request_url.encoded_host_and_port());

   auto nva = boost::container::small_vector<nghttp3_nv, 16>();
   nva.reserve(4 + std::distance(headers.begin(), headers.end()));
   nva.push_back(make_nv(":method", method_str));
   nva.push_back(make_nv(":scheme", scheme));
   nva.push_back(make_nv(":path", target));
   nva.push_back(make_nv(":authority", authority));

   for (auto&& item : headers)
   {
      if (item.name_string().starts_with(':'))
         mlogw("async_submit: invalid header '{}': setting pseudo headers is not allowed",
               item.name_string());

      nva.push_back(make_nv(item.name_string(), item.value()));
   }

   mlogd("{} {}", method_str, request_url.buffer());
   return submit_headers(nva, true /* request */);
}

void Http3ClientStream::async_get_response(client::Request::GetResponseHandler&& handler)
{
   if (response_delivered)
   {
      complete_immediately(std::move(handler), get_executor(), errors::already_started,
                           client::Response{nullptr});
      return;
   }

   on_cancel(handler, [this] {
      mlogd("async_get_response: cancelled");
      if (response_handler)
         complete_later(std::move(response_handler), get_executor(), errors::canceled,
                        client::Response{nullptr});
   });

   //
   // Delivering the response resumes the caller, which may drop the last reference to this stream
   // right there (its Request going out of scope with the Response never read). Keep it alive
   // until this function returns.
   //
   auto self = shared_from_this();

   response_handler = std::move(handler);
   deliver_response();

   //
   // Nothing will ever arrive on a stream that is already dead, so answer right away instead of
   // waiting for a response that cannot come -- the same reasoning that makes call_read_handler()
   // report the truncation to a read issued after the close.
   //
   if (closed)
      deliver_failure();
}

void Http3ClientStream::deliver_response()
{
   if (!headers_received || !response_handler)
      return;

   response_delivered = true;
   auto response = client::Response{std::make_unique<Http3ResponseReader>(*this)};
   swap_and_invoke(response_handler, error_code{}, std::move(response));
}

// =================================================================================================
// Http3ClientApplication implementation
// =================================================================================================

std::shared_ptr<quic::QuicStream> Http3ClientApplication::make_stream(int64_t id)
{
   return std::make_shared<Http3ClientStream>(*this, id);
}

void Http3ClientApplication::async_submit(SubmitHandler&& handler, std::string_view method,
                                          boost::urls::url url, const Fields& headers)
{
   int64_t stream_id = -1;
   if (auto rv = ngtcp2_conn_open_bidi_stream(session_.conn(), &stream_id, nullptr); rv != 0)
   {
      //
      // The peer's stream limit is reached: there is no stream to put this request on until one of
      // the requests in progress is finished, and the caller may well be the one who has to finish
      // it, so waiting here could deadlock. That makes it would_block, the same answer HTTP/1.1
      // gives for its limit of one and plain QUIC streams give for this very error, see
      // Session::async_submit() and quic::Connection::open_stream().
      //
      if (rv == NGTCP2_ERR_STREAM_ID_BLOCKED)
      {
         mlogd("async_submit: no stream available, peer's limit reached");
         std::move(handler)(errors::would_block, client::Request{nullptr});
         return;
      }

      mloge("async_submit: ngtcp2_conn_open_bidi_stream: {}", ngtcp2_strerror(rv));
      std::move(handler)(make_error_code(errc::invalid_argument), client::Request{nullptr});
      return;
   }

   auto* stream = static_cast<Http3ClientStream*>(session_.create_stream(stream_id));
   if (!stream->submit_request(method, url, headers))
   {
      session_.erase_stream(stream_id);
      std::move(handler)(make_error_code(errc::invalid_argument), client::Request{nullptr});
      return;
   }

   logd("[{}] async_submit: new stream ID: {}", stream->log_prefix_, stream_id);
   session_.wake_write();

   complete_later(std::move(handler), session_.get_executor(), error_code{},
                  client::Request{std::make_unique<Http3ClientWriter>(*stream)});
}

// =================================================================================================
// Entry point used by Client::Impl::async_connect() for Protocol::h3.
// =================================================================================================

Task<std::shared_ptr<Session::Impl>> async_connect_http3(Executor executor, std::string host,
                                                         std::string port, const Config& config)
{
   //
   // The certificate has to be valid for the origin, the host of the URL -- also when `host` is
   // an alternative service it advertised (RFC 7838, section 2.1).
   //
   quic::ClientConfig quic_config{.alpn = "h3",
                                  .host = config.url.host_address(),
                                  .tls_ca_file = config.tls_ca_file,
                                  .idle_timeout = 30s};
   const size_t max_header_size = config.max_header_size;
   co_return co_await quic::async_connect(
      std::move(executor), std::move(host), std::move(port), std::move(quic_config),
      [max_header_size](quic::QuicSession& session) -> std::unique_ptr<quic::QuicApplication> {
         return std::make_unique<Http3ClientApplication>(session, max_header_size);
      });
}

// =================================================================================================

} // namespace anyhttp::client
