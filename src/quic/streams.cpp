//
// Plain QUIC streams on a QUIC connection, see anyhttp/quic/streams.hpp.
//
#include "anyhttp/quic/streams.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/quic/backend.hpp"
#include "anyhttp/quic/session.hpp"

#include <boost/system/detail/errc.hpp>

#include <ngtcp2/ngtcp2.h>

#include <memory>
#include <utility>

namespace anyhttp::quic
{

namespace
{

/// A Stream handle on `stream`: one adapter for each direction.
Stream make_handle(PlainStream& stream)
{
   return Stream{std::make_shared<QuicReader<Reader::Impl>>(stream),
                 std::make_shared<QuicWriter<Writer::Impl>>(stream)};
}

} // namespace

// =================================================================================================

PlainStream::PlainStream(StreamApplication& app, int64_t stream_id)
   : QuicStream(app.session(), stream_id,
                app.config().zero_copy ? WriteMode::ZeroCopy : WriteMode::Staged),
     application(app)
{
}

PlainStream::~PlainStream() { detach_all(); }

void PlainStream::on_writable() { application.schedule(*this); }

uint64_t PlainStream::reset_error_code() const noexcept
{
   return application.config().reset_error_code;
}

uint64_t PlainStream::stop_sending_error_code() const noexcept
{
   return application.config().stop_sending_error_code;
}

// =================================================================================================

StreamApplication::StreamApplication(QuicSession& session, Config config, ConnectionHandler handler)
   : QuicApplication(session), config_(std::move(config)), handler_(std::move(handler))
{
}

StreamApplication::~StreamApplication() = default;

const std::string& StreamApplication::log_prefix() const noexcept { return session_.log_prefix(); }

std::shared_ptr<PlainStream> StreamApplication::find_stream(int64_t id)
{
   // every stream of this session was made by make_stream()
   return std::static_pointer_cast<PlainStream>(session_.find_stream(id));
}

bool StreamApplication::remote_stream(int64_t id) const noexcept
{
   return ngtcp2_conn_is_local_stream2(session_.conn(), id) == 0;
}

// -------------------------------------------------------------------------------------------------

void StreamApplication::fill_transport_params(ngtcp2_transport_params& params)
{
   params.initial_max_streams_bidi = config_.max_streams;
   params.initial_max_streams_uni = 0; // bidirectional streams only, so far
   params.initial_max_stream_data_bidi_local = config_.max_stream_data;
   params.initial_max_stream_data_bidi_remote = config_.max_stream_data;
   params.initial_max_stream_data_uni = 0;
   params.initial_max_data = config_.max_data;
}

int StreamApplication::on_ready()
{
   mlogd("plain streams ready");
   if (!handler_)
      return 0;

   //
   // The server's side: the connection goes to the handler registered for its ALPN, on the
   // session's executor, and lives as long as the handler holds on to it. Posted: this is reached
   // from inside ngtcp2, and the session counts as ready only once it returns.
   //
   auto session = std::static_pointer_cast<QuicSession>(session_.shared_from_this());
   run_later(session_.get_executor(), [this, session = std::move(session)]() mutable {
      if (closed_ec_)
         return; // gone before it got going
      auto connection = Connection{std::make_shared<Connection::Impl>(std::move(session), *this)};
      launch(session_.get_executor(), handler_(std::move(connection)),
             [prefix = session_.log_prefix()](const std::exception_ptr& ex) {
                if (ex)
                   logw("[{}] connection handler: {}", prefix, what(ex));
             });
   });
   return 0;
}

std::shared_ptr<QuicStream> StreamApplication::make_stream(int64_t id)
{
   return std::make_shared<PlainStream>(*this, id);
}

void StreamApplication::on_closed(error_code ec)
{
   if (closed_ec_)
      return;
   mlogd("plain streams closed: {}", ec.message());

   //
   // An accept_stream() waiting learns that no stream will ever come, the streams nobody has
   // accepted yet are dropped -- the session fails them, if it is still around to do so.
   //
   closed_ec_ = errors::connection_aborted;
   accept_queue_.clear();
   write_queue_.clear();
   if (accept_handler_)
      complete_later(std::move(accept_handler_), session_.get_executor(), closed_ec_, Stream{});
}

// -------------------------------------------------------------------------------------------------
// Accepting and opening streams
// -------------------------------------------------------------------------------------------------

std::shared_ptr<PlainStream> StreamApplication::open_remote_stream(int64_t id)
{
   if (auto stream = find_stream(id))
      return stream;

   //
   // A stream ngtcp2 tells us about that we don't know is one the peer opened just now -- also
   // when it is opened implicitly, by a later stream of the peer's arriving first, which gets no
   // stream_open callback of its own. Our own streams are known from the start, and none is
   // forgotten before ngtcp2 is done with it.
   //
   if (!remote_stream(id) || closed_ec_)
      return nullptr;

   session_.create_stream(id);
   auto stream = find_stream(id);
   mlogd("stream {} opened by the peer", id);
   accept_queue_.push_back(stream);
   deliver_accepted();
   return stream;
}

void StreamApplication::deliver_accepted()
{
   if (!accept_handler_ || accept_queue_.empty())
      return;

   auto stream = std::move(accept_queue_.front());
   accept_queue_.pop_front();
   // posted: this is reached from inside ngtcp2
   complete_later(std::move(accept_handler_), session_.get_executor(), error_code{},
                  make_handle(*stream));
}

void StreamApplication::async_accept_stream(AcceptStreamHandler&& handler)
{
   if (accept_handler_)
   {
      complete_immediately(std::move(handler), session_.get_executor(), errors::already_started,
                           Stream{});
      return;
   }

   if (!accept_queue_.empty())
   {
      auto stream = std::move(accept_queue_.front());
      accept_queue_.pop_front();
      complete_immediately(std::move(handler), session_.get_executor(), error_code{},
                           make_handle(*stream));
      return;
   }

   if (closed_ec_)
   {
      complete_immediately(std::move(handler), session_.get_executor(), closed_ec_, Stream{});
      return;
   }

   on_cancel(handler, [this] {
      if (accept_handler_)
         complete_later(std::move(accept_handler_), session_.get_executor(), errors::canceled,
                        Stream{});
   });
   accept_handler_ = std::move(handler);
}

std::tuple<error_code, Stream> StreamApplication::open_stream()
{
   if (closed_ec_ || session_.closed() || !session_.application_ready())
      return {errors::connection_aborted, Stream{}};

   int64_t id = -1;
   if (auto rv = ngtcp2_conn_open_bidi_stream(session_.conn(), &id, nullptr); rv != 0)
   {
      if (rv == NGTCP2_ERR_STREAM_ID_BLOCKED)
         return {errors::would_block, Stream{}};
      mloge("ngtcp2_conn_open_bidi_stream: {}", ngtcp2_strerror(rv));
      return {errors::connection_aborted, Stream{}};
   }

   auto* stream = static_cast<PlainStream*>(session_.create_stream(id));
   mlogd("stream {} opened", id);
   return {error_code{}, make_handle(*stream)};
}

// -------------------------------------------------------------------------------------------------
// From ngtcp2, through the session
// -------------------------------------------------------------------------------------------------

int StreamApplication::stream_open(int64_t id)
{
   open_remote_stream(id);
   return 0;
}

int StreamApplication::recv_stream_data(int64_t id, std::span<const uint8_t> data, bool fin)
{
   //
   // Connection credit right away, stream credit only as the stream's Reader reads it -- see
   // QuicSession::consume_stream().
   //
   ngtcp2_conn_extend_max_offset(session_.conn(), data.size());

   auto stream = open_remote_stream(id);
   if (!stream || stream->read_aborted)
      return 0;

   stream->on_data_chunk(data.data(), data.size());
   if (fin)
      stream->on_eof();
   return 0;
}

int StreamApplication::acked_stream_data(int64_t id, uint64_t offset, uint64_t len)
{
   if (auto stream = find_stream(id))
      stream->on_acked(offset + len);
   return 0;
}

int StreamApplication::stream_close(int64_t id, uint32_t flags, uint64_t app_error_code)
{
   if (auto stream = find_stream(id))
   {
      //
      // Both directions are over. Whatever is still pending can't complete any more: a stream that
      // was reset either way did not get all of its body across.
      //
      const bool reset = (flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET) != 0;
      mlogd("stream {} closed{}", id, reset ? std::format(" (error {})", app_error_code) : "");
      stream->fail(reset ? make_error_code(errc::connection_reset) : error_code{});
   }

   // the peer may open another one in its place
   if (remote_stream(id))
      ngtcp2_conn_extend_max_streams_bidi(session_.conn(), 1);
   return 0;
}

int StreamApplication::stream_reset(int64_t id, uint64_t app_error_code)
{
   if (auto stream = open_remote_stream(id))
   {
      mlogd("stream {} reset by the peer (error {})", id, app_error_code);
      stream->abort_reading();
   }
   return 0;
}

int StreamApplication::stream_stop_sending(int64_t id, uint64_t /*app_error_code*/)
{
   // we stopped reading, see QuicSession::stop_reading()
   if (auto stream = find_stream(id))
      stream->abort_reading();
   return 0;
}

int StreamApplication::recv_stop_sending(int64_t id, uint64_t app_error_code)
{
   if (auto stream = open_remote_stream(id))
   {
      mlogd("stream {}: peer stopped reading (error {})", id, app_error_code);
      stream->abort_writing(make_error_code(errc::connection_reset));
   }
   return 0;
}

int StreamApplication::extend_max_stream_data(int64_t id)
{
   if (auto stream = find_stream(id))
   {
      stream->blocked = false;
      schedule(*stream);
   }
   return 0;
}

// -------------------------------------------------------------------------------------------------
// Write scheduling
// -------------------------------------------------------------------------------------------------

void StreamApplication::schedule(PlainStream& stream, bool wake)
{
   if (stream.scheduled || stream.blocked || closed_ec_)
      return;
   stream.scheduled = true;
   write_queue_.push_back(stream.id);
   if (wake)
      session_.wake_write();
}

void StreamApplication::unschedule_front(PlainStream& stream)
{
   if (!write_queue_.empty() && write_queue_.front() == stream.id)
      write_queue_.pop_front();
   stream.scheduled = false;
}

//
// The stream at the front of the write queue gets the packet. It stays there until ngtcp2 has
// told what became of its data -- a packet that had no room for any of it leaves it in place for
// the next one -- and then goes to the back, if it has more.
//
ngtcp2_ssize StreamApplication::next_stream_data(int64_t& id, bool& fin, std::span<ngtcp2_vec> vec)
{
   while (!write_queue_.empty())
   {
      auto stream = find_stream(write_queue_.front());
      if (!stream)
      {
         write_queue_.pop_front();
         continue;
      }

      if (stream->blocked)
      {
         unschedule_front(*stream);
         continue;
      }

      bool stream_fin = false;
      const auto n = stream->pending_data(stream->tx_sent, vec, stream_fin);
      if (n == 0 && (!stream_fin || stream->fin_offered))
      {
         unschedule_front(*stream); // nothing (more) to send, until on_writable()
         continue;
      }

      id = stream->id;
      fin = stream_fin;
      return static_cast<ngtcp2_ssize>(n);
   }
   return 0;
}

int StreamApplication::on_stream_data_written(int64_t id, size_t n, bool fin)
{
   auto stream = find_stream(id);
   if (!stream)
      return 0;

   unschedule_front(*stream);

   //
   // ngtcp2 holds on to what it wrote, and to nothing else: what it was offered and did not
   // write, it will be offered again.
   //
   stream->on_offered(stream->tx_sent + n, fin);
   stream->on_sent(stream->tx_sent + n);

   schedule(*stream, false); // to the back, round robin; dropped if there is nothing more
   return 0;
}

void StreamApplication::on_stream_blocked(int64_t id)
{
   if (auto stream = find_stream(id))
   {
      stream->blocked = true; // until extend_max_stream_data()
      unschedule_front(*stream);
   }
}

void StreamApplication::on_stream_shut_wr(int64_t id)
{
   if (auto stream = find_stream(id))
   {
      unschedule_front(*stream);
      stream->abort_writing(make_error_code(errc::connection_reset));
   }
}

// =================================================================================================

Connection::Impl::~Impl() { session->close_connection(0); }

// =================================================================================================

ApplicationFactory make_stream_application(Config config, ConnectionHandler handler)
{
   return [config = std::move(config),
           handler = std::move(handler)](QuicSession& session) -> std::unique_ptr<QuicApplication> {
      return std::make_unique<StreamApplication>(session, config, handler);
   };
}

// =================================================================================================

} // namespace anyhttp::quic
