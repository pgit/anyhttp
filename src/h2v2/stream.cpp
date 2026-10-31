#include "anyhttp/h2v2/stream.hpp"

#include "anyhttp/client.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/h2v2/common.hpp"
#include "anyhttp/h2v2/session.hpp"
#include "anyhttp/request_handlers.hpp" // IWYU pragma: keep

#include <boost/asio/buffer.hpp>

#include <boost/beast/http/error.hpp>
#include <boost/beast/http/status.hpp>

#include <boost/system/detail/errc.hpp>
#include <boost/system/errc.hpp>

#include <boost/container/small_vector.hpp>

#include <nghttp2v2/nghttp2.h>

#include <ranges>
#include <utility>

namespace anyhttp::nghttp2
{

// =================================================================================================

template <typename Base>
NGHttp2Reader<Base>::NGHttp2Reader(NGHttp2Stream& stream)
   : stream(&stream), executor(stream.get_executor())
{
   stream.reader = this;
}

template <typename Base>
NGHttp2Reader<Base>::~NGHttp2Reader()
{
   if (stream)
   {
      stream->reader = nullptr;
      stream->delete_reader();
   }
}

template <typename Base>
void NGHttp2Reader<Base>::detach()
{
   //
   // The stream is going away first (session teardown, or the stream close outliving this
   // exchange). Remember how the body stood, so that reads issued from now on keep answering per
   // the Reader contract: a body that was read to its clean end keeps reporting \c eof, anything
   // else is a truncation.
   //
   assert(stream);
   detached_ec = stream->reading_finished() ? errors::eof : errors::partial_message;
   detached_log_prefix = stream->log_prefix();
   detached_fields = std::move(stream->fields); // the stream is being destroyed
   stream = nullptr;
}

template <typename Base>
std::string NGHttp2Reader<Base>::log_prefix() const
{
   return stream ? stream->log_prefix() : detached_log_prefix;
}

// -------------------------------------------------------------------------------------------------

template <typename Base>
Executor NGHttp2Reader<Base>::get_executor() const noexcept
{
   return executor;
}

template <typename Base>
const Fields& NGHttp2Reader<Base>::fields() const
{
   return stream ? stream->fields : detached_fields;
}

template <typename Base>
std::optional<size_t> NGHttp2Reader<Base>::content_length() const noexcept
{
   if (!stream)
      return std::nullopt;
   return stream->content_length;
}

template <typename Base>
void NGHttp2Reader<Base>::async_read_some(boost::asio::mutable_buffer buffer,
                                          ReadSomeHandler&& handler)
{
   //
   // An empty buffer is not a request to read anything: complete right away, without looking at
   // whether the body has ended or the stream is even still there -- as ASIO does for a
   // zero-length read.
   //
   if (asio::buffer_size(buffer) == 0)
   {
      complete_immediately(std::move(handler), get_executor(), error_code{}, size_t{0});
      return;
   }

   //
   // The stream is gone; detach() latched how the body stood at that point, so a cleanly finished
   // body keeps reporting eof (as the Reader contract requires) and a truncated one keeps
   // reporting partial_message.
   //
   if (!stream)
   {
      // Reading on after a cleanly finished body is part of the contract, not worth a warning.
      if (detached_ec == errors::eof)
         mlogd("async_read_some: stream already gone ({})", detached_ec.message());
      else
         mlogw("async_read_some: stream already gone ({})", detached_ec.message());
      complete_immediately(std::move(handler), get_executor(), detached_ec, size_t{0});
      return;
   }

   on_cancel(handler, [this] {
      logd("[{}] async_read_some: \x1b[1;31m{}\x1b[0m", stream->log_prefix(), "cancelled");
      if (stream->read_handler_)
         complete_later(std::move(stream->read_handler_), get_executor(), errors::canceled, 0);
   });

   assert(!stream->read_handler_);
   logd("[{}] async_read_some:", stream->log_prefix());
   stream->read_handler_buffer_ = buffer;
   stream->read_handler_ = std::move(handler);
   stream->call_read_handler();
}

// =================================================================================================

namespace
{

//
// The two roles a reader can be in. Everything above is the same for both; what they add is the
// half of the incoming message that only their role has -- a request line, or a status code.
// Like the fields, it is moved out of the stream on detach(): a handler may well look at the
// request line only after the session is gone, e.g. when it first runs during shutdown.
//

class NGHttp2RequestReader final : public NGHttp2Reader<server::Request::Impl>
{
public:
   using NGHttp2Reader<server::Request::Impl>::NGHttp2Reader;

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
      NGHttp2Reader::detach();
   }

private:
   std::string detached_method;
   boost::urls::url detached_url;
};

class NGHttp2ResponseReader final : public NGHttp2Reader<client::Response::Impl>
{
public:
   using NGHttp2Reader<client::Response::Impl>::NGHttp2Reader;

   unsigned int status_code() const noexcept override
   {
      return stream ? stream->status_code.value_or(0) : detached_status_code;
   }

   void detach() override
   {
      assert(stream);
      detached_status_code = stream->status_code.value_or(0);
      NGHttp2Reader::detach();
   }

private:
   unsigned int detached_status_code = 0;
};

} // namespace

// =================================================================================================

template <typename Base>
NGHttp2Writer<Base>::NGHttp2Writer(NGHttp2Stream& stream)
   : stream(&stream), executor(stream.get_executor())
{
   stream.writer = this;
}

template <typename Base>
NGHttp2Writer<Base>::~NGHttp2Writer()
{
   if (stream)
   {
      stream->writer = nullptr;
      stream->delete_writer();
   }
}

template <typename Base>
void NGHttp2Writer<Base>::detach()
{
   // remember whether the body was cleanly ended, so writes issued after this still answer
   // per the Writer contract -- see async_write() below
   assert(stream);
   detached_eof_submitted = stream->eof_submitted;
   detached_log_prefix = stream->log_prefix();
   stream = nullptr;
}

template <typename Base>
std::string NGHttp2Writer<Base>::log_prefix() const
{
   return stream ? stream->log_prefix() : detached_log_prefix;
}

// -------------------------------------------------------------------------------------------------

template <typename Base>
Executor NGHttp2Writer<Base>::get_executor() const noexcept
{
   return executor;
}

template <typename Base>
void NGHttp2Writer<Base>::content_length(std::optional<size_t> content_length)
{
   content_length_ = content_length;
}

namespace
{
nghttp2_ssize stream_read_data(nghttp2_conn*, int64_t /*stream_id*/, nghttp2_vec* vec,
                               size_t veccnt, uint32_t* pflags, void* /*conn_user*/,
                               void* stream_user)
{
   auto stream = static_cast<NGHttp2Stream*>(stream_user);
   assert(stream);
   return stream->data_reader(vec, veccnt, pflags);
}
} // namespace

nghttp2_data_reader make_data_reader()
{
   nghttp2_data_reader dr{};
   dr.read_data = &stream_read_data;
   return dr;
}

template <typename Base>
void NGHttp2Writer<Base>::async_submit(StatusHandler&& handler, unsigned int status_code,
                                       const Fields& headers)
{
   if (!stream)
   {
      mlogw("async_submit: stream already gone");
      std::move(handler)(errors::connection_aborted);
      return;
   }

   using namespace boost::beast::http;
   logd("[{}] {} {}", stream->log_prefix(), status_code,
        obsolete_reason(int_to_status(status_code)));

   const auto status_code_str = std::to_string(status_code);
   const auto date = http_date_now();

   auto nva = boost::container::small_vector<nghttp2_nv, 16>();
   nva.reserve(4 + std::distance(headers.begin(), headers.end()));
   nva.push_back(make_nv_ls(":status", status_code_str));
   nva.push_back(make_nv_ls("date", date));

   //
   // Point the client at our HTTP/3 endpoint, see server::Config::alt_svc_max_age. Only a server
   // session has one of these, and a handler that names the field itself gets its way: HTTP/2
   // would happily carry both, which is not what "Alt-Svc" means.
   //
   if (const auto& alt_svc = stream->parent.alt_svc_; !alt_svc.empty() && !headers.count("alt-svc"))
      nva.push_back(make_nv_ls("alt-svc", alt_svc));

   for (auto&& item : headers)
   {
      if (item.name_string().starts_with(':'))
         logw("[{}] async_submit: invalid header '{}'", stream->log_prefix(), item.name_string());

      nva.push_back(make_nv_ls(item.name_string(), item.value()));
   }

   std::string length_str;
   if (content_length_)
   {
      length_str = std::to_string(*content_length_);
      nva.push_back(make_nv_ls("content-length", length_str));
   }

   for (auto nv : nva)
      logd("[{}]   \x1b[1;34m{}\x1b[0m: {}", stream->log_prefix(), truncated(name_of(nv)),
           truncated(value_of(nv)));

   //
   // nghttp2 pulls the body out of the stream through data_reader(), with the stream itself as the
   // per-stream user data it hands back. The reader is copied into the stream's send queue, so a
   // local is enough. TODO: pass nullptr for a response that is known to have no body, which
   // ends the stream without a DATA frame.
   //
   auto dr = make_data_reader();

   if (auto rv = nghttp2_conn_submit_response(stream->parent.conn, stream->id, nva.data(),
                                              nva.size(), &dr))
   {
      mloge("async_submit: nghttp2_conn_submit_response: {}", nghttp2_strerror(rv));
      std::move(handler)(errors::connection_aborted);
      return;
   }

   stream->parent.start_write();

   std::move(handler)(error_code{});
}

template <typename Base>
void NGHttp2Writer<Base>::async_write(WriteHandler&& handler, asio::const_buffer buffer, bool eof)
{
   if (stream)
   {
      stream->async_write(std::move(handler), buffer, eof);
      return;
   }

   //
   // The stream is gone, but the entry ladder of the Writer contract still applies, answered
   // from the state detach() latched: an empty non-EOF write stays a free no-op, a body that was
   // cleanly ended keeps answering as such -- bare re-end idempotent, data broken_pipe -- and
   // only a stream that vanished mid-body is a connection error.
   //
   const bool empty = asio::buffer_size(buffer) == 0;
   error_code ec;
   if (empty && !eof)
      ec = {};
   else if (detached_eof_submitted)
      ec = empty ? error_code{} : make_error_code(errc::broken_pipe);
   else
   {
      mlogw("async_write: stream already gone");
      ec = errors::connection_aborted;
   }
   complete_immediately(std::move(handler), executor, ec);
}

template <typename Base>
void NGHttp2Writer<Base>::async_get_response(client::Request::GetResponseHandler&& handler)
{
   if (!stream)
   {
      mlogw("async_get_response: stream already gone");
      std::move(handler)(errors::connection_aborted, client::Response{nullptr});
   }
   else
      stream->async_get_response(std::move(handler));
}

// =================================================================================================

NGHttp2Stream::NGHttp2Stream(NGHttp2Session& parent, int64_t id_, WriteMode write_mode)
   : write_mode(write_mode), parent(parent), id(id_)
{
   mlogd("\x1b[1;33mStream: ctor\x1b[0m");
}

std::string NGHttp2Stream::log_prefix() const { return parent.log_prefix(id); }

size_t NGHttp2Stream::read_buffers_size() const
{
   return asio::buffer_size(read_buffer_) + // remaining part of first buffer
          std::ranges::fold_left( // sum of all other buffers
             pending_read_buffers_ | //
                std::views::drop(1) |
                std::views::transform([](const auto& buffer) { return buffer.size(); }),
             size_t{0}, std::plus{});
}

void NGHttp2Stream::call_read_handler(asio::const_buffer view)
{
   Defer finish_close([this] { finish_deferred_close(); });

   //
   // If there is no pending read handler we can invoke now, we have to buffer the data until there
   // is. The amount of data that is buffered is limited by the window size, which is only reopened
   // below, once the data has actually been delivered.
   //
   if (!read_handler_)
   {
      if (!is_empty(view))
      {
         pending_read_buffers_.emplace_back(make_buffer(view));
         if (pending_read_buffers_.size() == 1)
            read_buffer_ = asio::buffer(pending_read_buffers_.front());
      }
      mlogd("read_callback: no pending read handler... ({} buffers and {} bytes pending)",
            pending_read_buffers_.size(), read_buffers_size());
      return;
   }

   //
   // Avoid recursion. In this function, we invoke the read handler, which may resume a
   // user-provided coroutine. That coroutine is likely to call async_read_some() again,
   // resulting in another call to this function.
   //
   if (inside_call_read_handler_)
   {
      mlogd("read_callback: avoided recursion, returning...");
      return;
   }

   Defer no_recurse([&]() { inside_call_read_handler_ = false; });
   inside_call_read_handler_ = true;

   //
   // If there is no pending data to write, we can start writing the new view right away.
   //
   if (pending_read_buffers_.empty())
      std::swap(read_buffer_, view); // view is empty after this

   //
   // Try to deliver as many buffers as possible. As long as the consumer installs a new read
   // handler after handling a buffer, this loop can continue.
   //
   size_t count = 0, consumed = 0;
   while (read_handler_ && !is_empty(read_buffer_))
   {
      size_t copied = asio::buffer_copy(read_handler_buffer_, read_buffer_);
      count++, consumed += copied;
      bytesRead += copied;
      read_buffer_ += copied;

      if (count == 1)
         mlogd("read_callback: calling read handler with {} bytes... (buf={})", copied,
               asio::buffer_size(read_buffer_));
      else
         mlogd("read_callback: calling read handler with {} bytes... (#{} in a row, buf={})",
               copied, count, asio::buffer_size(read_buffer_));

      //
      // swap_and_invoke() moves the read handler into a local variable before invoking it,
      // allowing a new read handler to be set. This is what we call 'respawning' here, when a
      // new handler is set immediately.
      //
      swap_and_invoke(read_handler_, error_code{}, copied);

      if (read_handler_)
         mlogd("read_callback: calling handler with {} bytes... done,"
               " RESPAWNED ({} buffers pending)",
               copied, pending_read_buffers_.size());
      else
         mlogd("read_callback: calling handler with {} bytes... done", copied);

      //
      // Advance to next stored buffer. If there is none, start writing the view that has been
      // passed to this function.
      //
      if (is_empty(read_buffer_))
      {
         if (!pending_read_buffers_.empty())
            pending_read_buffers_.pop_front();

         if (!pending_read_buffers_.empty())
            read_buffer_ = asio::buffer(pending_read_buffers_.front());
         else
            std::swap(read_buffer_, view); // clear view so that we don't write it twice
      }
   }

   //
   // To apply back pressure, the stream window is reopened AFTER the handler is invoked. As always,
   // this is accompanied by a start_write() because this might un-block flow control.
   //
   // The connection window was already reopened where the data arrived, see on_recv_data(): only
   // the stream-level credit is what the reader's pace is allowed to hold back.
   //
   if (consumed)
   {
      nghttp2_conn_extend_max_stream_offset(parent.conn, id, consumed);
      mlogd("consumed {} bytes", consumed);
      parent.start_write();
   }

   mlogd("read_callback: finished, {} buffers pending, eof_received={}",
         pending_read_buffers_.size(), eof_received);

   //
   // Buffer remaining data from 'view' passed into this function.
   //
   if (!is_empty(read_buffer_))
   {
      assert(!read_handler_);
      if (pending_read_buffers_.empty())
      {
         auto& buffer = pending_read_buffers_.emplace_back(make_buffer(read_buffer_));
         read_buffer_ = asio::buffer(buffer);
      }
      else if (!is_empty(view))
      {
         pending_read_buffers_.emplace_back(make_buffer(view));
      }
   }

   //
   // No data left and the peer is done: report the end of the body the way ASIO does everywhere
   // else. Keep reporting it for as long as reads keep being issued -- a handler may re-arm from
   // within, and every read past the end of a body ends the same way.
   //
   else if (eof_received)
   {
      while (read_handler_)
      {
         mlogd("read_callback: delivering EOF...");
         swap_and_invoke(read_handler_, errors::eof, 0);
         mlogd("read_callback: delivering EOF... done");
      }
      return;
   }

   //
   // If there is no further user-provided read handler to call, we can return for now.
   //
   if (!read_handler_)
      return;

   if (reading_finished() || closed)
      swap_and_invoke(read_handler_, errors::partial_message, 0);
}

void NGHttp2Stream::on_data(const uint8_t* data, size_t len)
{
   assert(!eof_received);
   assert(len); // for EOF, on_eof() is called instead

   mlogd("read callback: {} bytes...", len);

   //
   // We try to invoke the read handler directly with the new buffer view. Only if there is no
   // pending read handler that can be invoked immediately, the data will be copied into a buffer.
   //
   call_read_handler(asio::const_buffer(data, len));
}

void NGHttp2Stream::on_eof()
{
   assert(!eof_received);

   mlogd("read callback: EOF");

   eof_received = true;
   call_read_handler({/* empty buffer */});
}

NGHttp2Stream::~NGHttp2Stream()
{
   mlogd("\x1b[33mStream: dtor... \x1b[0m");
   if (reader)
   {
      mlogd("Stream: dtor... detaching reader");
      reader->detach();
   }
   if (writer)
   {
      mlogd("Stream: dtor... detaching writer");
      writer->detach();
   }
   if (response_handler)
   {
      mlogw("Stream: dtor... cancelling async_get_response()");
      swap_and_invoke(response_handler, errors::canceled, client::Response{nullptr});
   }
   if (read_handler_)
   {
      mlogw("Stream: dtor... cancelling async_read_some()");
      swap_and_invoke(read_handler_, errors::partial_message, bytesRead);
   }
   if (write_handler)
   {
      mlogw("Stream: dtor... cancelling async_write()");
      swap_and_invoke(write_handler, errors::canceled);
   }

   mlogd("\x1b[33mStream: dtor... done\x1b[0m");
}

// =================================================================================================

void NGHttp2Stream::async_write(WriteHandler handler, asio::const_buffer buffer, bool eof)
{
   const bool empty = buffer.size() == 0;

   //
   // The protocol-independent entry ladder, in the order the Writer contract in common.hpp
   // prescribes: a zero-length non-EOF write is a free no-op wherever the body stands; after the
   // body has been ended, data has no body left to belong to -- through either entry point --
   // while a bare re-end is answered by how far the end has actually gotten.
   //
   if (empty && !eof)
   {
      complete_immediately(std::move(handler), get_executor(), error_code{});
      return;
   }

   if (eof_requested)
   {
      if (!empty)
      {
         mloge("async_write: body has already been ended");
         complete_immediately(std::move(handler), get_executor(),
                              make_error_code(errc::broken_pipe));
         return;
      }

      //
      // A bare re-end. If nghttp2 already knows about the end, there is nothing left to do. If
      // not -- a cancelled async_write_eof() leaves the intent standing but takes its handler
      // away, so the flag never reached the data reader -- fall through to the normal path below:
      // the re-issued write picks up where the cancelled one left off, which is what lets an
      // upload be ended once the peer's flow control window reopens.
      //
      if (eof_submitted)
      {
         complete_immediately(std::move(handler), get_executor(), error_code{});
         return;
      }
   }

   //
   // The same code as a write that was parked when the stream closed, see close_stream(): which
   // of the two a writer gets is only a matter of whether it was resumed before the RST_STREAM
   // was read.
   //
   if (closed)
   {
      mlogw("async_write: stream already closed");
      complete_immediately(std::move(handler), get_executor(),
                           make_error_code(errc::connection_reset));
      return;
   }

   assert(!write_handler);

   mlogd("async_write: buffer={} eof={} is_deferred={}", buffer.size(), eof, is_deferred);

   write_buffer = buffer;
   offered_ = 0;
   written_ = 0;
   eof_requested |= eof;
   write_handler = std::move(handler);

   on_cancel(write_handler, [this] {
      mlogd("async_write: \x1b[1;31m{}\x1b[0m", "cancelled");

      //
      // ZeroCopy with bytes still outstanding: nghttp2 is pointing into the caller's buffer, which
      // this handler is about to hand back, and there is no way to un-offer it. So the stream is
      // reset, and the handler completes from close_stream() instead -- by then the RST_STREAM has
      // been serialized and the DATA frame that was in progress finished, which is the point at
      // which nghttp2 has provably stopped reading. That is one send loop pass away, and needs no
      // flow control window, so it is no slower than completing here. The peer sees a body that
      // stops short of its end, which is what a cancelled write means.
      //
      // Resetting is also why a cancelled write is the end of this stream, where a Staged one can
      // be followed by another. See WriteMode.
      //
      if (write_mode == WriteMode::ZeroCopy && written_ < offered_ && !closed)
      {
         mlogd("async_write: {} of {} bytes still outstanding, resetting stream",
               offered_ - written_, offered_);
         write_cancelled_ = true;
         nghttp2_conn_shutdown_stream(parent.conn, 0, id, NGHTTP2_CANCEL);
         parent.start_write();
         return;
      }

      // make sure to post this -- otherwise "MAIN COROUTINE DID NOT COMPLETE" happens
      if (write_handler)
         complete_later(std::move(write_handler), get_executor(), errors::canceled);
   });

   resume();
}

// -------------------------------------------------------------------------------------------------

void NGHttp2Stream::resume()
{
   if (is_deferred)
   {
      mlogd("async_write: resuming stream ({} bytes to write)", write_buffer.size());

      //
      // It is important to reset the deferred state BEFORE resuming, because it may result in
      // an immediate call to the data reader.
      //
      is_deferred = false;
      nghttp2_conn_resume_stream(parent.conn, id);
      parent.start_write();
   }
}

// =================================================================================================

void NGHttp2Stream::async_get_response(client::Request::GetResponseHandler&& handler)
{
   mlogd("async_get_response:");

   assert(!response_handler);
   if (response_delivered)
   {
      auto ec = errors::already_started;
      mlogw("async_get_response: \x1b[1;31m{}\x1b[0m", what(ec));
      complete_immediately(std::move(handler), get_executor(), ec, client::Response{nullptr});
      return;
   }

   on_cancel(handler, [this] {
      mlogd("async_get_response: \x1b[1;31m{}\x1b[0m", "cancelled");
      if (response_handler)
         complete_later(std::move(response_handler), get_executor(), errors::canceled,
                        client::Response{nullptr});
   });

   response_handler = std::move(handler);
   deliver_response();
}

// -------------------------------------------------------------------------------------------------

//
// Version 2 takes the body by reference, where version 1's producer callback copied it into the
// frame nghttp2 was building. So this hands over a vector -- into the caller's own buffer, or into
// a copy of a piece of it, see WriteMode -- and nghttp2 reads from it until it has framed all of
// it, asking again only then.
//
nghttp2_ssize NGHttp2Stream::data_reader(nghttp2_vec* vec, size_t veccnt, uint32_t* pflags)
{
   assert(!is_deferred);

   //
   // No write in progress. Nothing may be handed over, and above all the body must not be ended
   // here: a cancelled async_write_eof() leaves \c eof_requested standing without a handler, and it
   // is the write that re-issues it which gets to end the body -- otherwise END_STREAM would go out
   // for a write that reported itself cancelled. Version 1 deferred here for the same reason.
   //
   // Returning no data without the EOF flag counts as a callback failure, hence WOULDBLOCK: the
   // stream waits until resume() wakes it.
   //
   if (!write_handler)
   {
      if (eof_submitted)
      {
         // asked again past the end of the body: say so once more
         *pflags |= NGHTTP2_READ_DATA_FLAG_EOF;
         return 0;
      }

      mlogd("write callback: nothing to send, DEFERRING");
      is_deferred = true;
      return NGHTTP2_ERR_WOULDBLOCK;
   }

   //
   // An empty write only ever ends the body, see async_write(). It has no bytes to be framed, so it
   // completes right here.
   //
   const size_t left = write_buffer.size() - offered_;
   if (left == 0)
   {
      if (!eof_requested || eof_submitted)
      {
         mlogd("write callback: nothing to send, DEFERRING");
         is_deferred = true;
         return NGHTTP2_ERR_WOULDBLOCK;
      }

      mlogd("write callback: EOF");
      eof_submitted = true;
      *pflags |= NGHTTP2_READ_DATA_FLAG_EOF;
      swap_and_invoke(write_handler, error_code{});
      return 0;
   }

   //
   // There is data to hand over, but nowhere to put it: wait to be asked again with room.
   //
   if (veccnt == 0)
   {
      mlogd("write callback: no vector to fill, DEFERRING");
      is_deferred = true;
      return NGHTTP2_ERR_WOULDBLOCK;
   }

   //
   // Hand over the caller's buffer itself, or a copy of the next chunk of it. nghttp2 asks again
   // only once it has framed everything it was given, so \c write_chunk_ may simply be overwritten.
   //
   const size_t offer = write_mode == WriteMode::ZeroCopy ? left : std::min(left, write_chunk_size);
   const auto* const base = static_cast<const uint8_t*>(write_buffer.data()) + offered_;
   if (write_mode == WriteMode::ZeroCopy)
      vec[0].base = const_cast<uint8_t*>(base);
   else
   {
      write_chunk_.assign(base, base + offer);
      vec[0].base = write_chunk_.data();
   }
   vec[0].len = offer;
   offered_ += offer;

   //
   // The last bytes of a write that ends the body carry END_STREAM themselves: setting the flag
   // on this very DATA frame is what spares an async_write_eof() with a payload the extra, empty
   // frame that ending a body would otherwise cost.
   //
   const bool last = offered_ == write_buffer.size();
   if (last && eof_requested)
   {
      mlogd("write callback: EOF along with the last {} bytes", offer);
      eof_submitted = true;
      *pflags |= NGHTTP2_READ_DATA_FLAG_EOF;
   }

   //
   // Staged: the caller's buffer has been copied out of, so it is free the moment the last chunk is
   // taken. ZeroCopy: nghttp2 is pointing into it, so the handler has to wait for
   // on_write_offset().
   //
   if (last && write_mode == WriteMode::Staged)
   {
      mlogd("write callback: copied the last {} bytes, running handler...", offer);
      swap_and_invoke(write_handler, error_code{});
   }
   else
      mlogd("write callback: offered {} bytes, {} left", offer, write_buffer.size() - offered_);

   return 1;
}

void NGHttp2Stream::on_write_offset(size_t len)
{
   bytesWritten += len;

   //
   // Only ZeroCopy has anything to do here: its write handler is waiting for the caller's buffer to
   // have been framed. A Staged write is long complete by the time its chunk reaches a frame -- the
   // offsets of one write arrive once the next has started, so counting them against it would be
   // counting the wrong write.
   //
   if (write_mode != WriteMode::ZeroCopy)
      return;

   written_ += len;
   mlogd("write offset: {} of {} bytes framed", written_, write_buffer.size());

   if (!write_handler || written_ < write_buffer.size())
      return;

   mlogd("write offset: running handler...");
   swap_and_invoke(write_handler, error_code{});
   if (write_handler)
      mlogd("write offset: running handler... done -- RESPAWNED");
   else
      mlogd("write offset: running handler... done");
}

void NGHttp2Stream::log_received_headers()
{
   for (const auto& [name, value] : received_headers)
      mlogd("  \x1b[1;34m{}\x1b[0m: {}", truncated(name), truncated(value));
   received_headers.clear();
}

void NGHttp2Stream::on_response()
{
   //
   // A response with too large a header section is of no use: fail it, and stop the server from
   // sending its body. The stream is kept until the failure has been delivered, see close_stream().
   //
   if (header_limit_exceeded)
   {
      response_error = errors::header_limit;
      nghttp2_conn_shutdown_stream(parent.conn, 0, id, NGHTTP2_CANCEL);
   }
   else
      has_response = true;

   deliver_response();
}

void NGHttp2Stream::deliver_response()
{
   if (!has_response && !response_error)
   {
      mlogd("deliver_response: no response, yet");
   }
   else if (!response_handler)
   {
      mlogd("deliver_response: not waiting for a response, yet");
   }
   else if (response_error)
   {
      mlogw("deliver_response: {}", what(response_error));
      response_delivered = true;
      swap_and_invoke(response_handler, response_error, client::Response{nullptr});

      // the stream may have been kept around just for this, see close_stream()
      if (closed)
         parent.close_stream(id);
   }
   else
   {
      response_delivered = true;
      auto impl = client::Response{std::make_unique<NGHttp2ResponseReader>(*this)};
      swap_and_invoke(response_handler, error_code{}, std::move(impl));
   }
}

// -------------------------------------------------------------------------------------------------

void NGHttp2Stream::on_request()
{
   server::Request request(std::make_unique<NGHttp2RequestReader>(*this));
   server::Response response(std::make_unique<NGHttp2Writer<server::Response::Impl>>(*this));

   auto& server = dynamic_cast<ServerReference&>(parent).server();
   if (header_limit_exceeded)
      launch(get_executor(), header_fields_too_large(std::move(request), std::move(response)));
   else if (auto& handler = server.request_handler())
      launch(get_executor(), handler(std::move(request), std::move(response)));
   else
   {
      mloge("on_request: no request handler!");
      launch(get_executor(), not_found(std::move(response)));
   }
}

Executor NGHttp2Stream::get_executor() const noexcept { return parent.get_executor(); }

// =================================================================================================

/**
 * This function is called if the "Request" (on the server side) is deleted. This means that we no
 * longer want to read DATA from the peer. However, we might still want to deliver a response, like
 * a 404. But if we reset the stream too early, this is stopped as well.
 *
 * The key is to NOT close the stream early, but rely on HTTP/2 flow control to eventually stop
 * any incoming DATA frames. The closing of the stream is delayed until after the response has been
 * delivered.
 *
 * On the client side, the same mechanism is applied.
 */
void NGHttp2Stream::delete_reader()
{
   mlogd("delete_reader");

   assert(!read_handler_);

   if (closed)
      mlogd("delete_reader: stream already closed");
   else if (reading_finished())
      mlogd("delete_reader: reading finished");
   else if (size_t pending = read_buffers_size())
   {
      mlogw("delete_reader: reading not finished, discarding {} pending bytes in {} buffers",
            pending, pending_read_buffers_.size());

      // Don't re-open the stream window here, or the peer will resume sending data to us.
      read_buffer_ = asio::const_buffer{};
      pending_read_buffers_.clear();
   }

   reader = nullptr;
   maybe_close_stream();
   finish_deferred_close();
}

void NGHttp2Stream::delete_writer()
{
   mlogd("delete_writer");

   if (closed)
      mlogd("delete_writer: stream already closed");
   else if (eof_submitted)
      mlogd("delete_writer: writing finished");

   writer = nullptr;
   maybe_close_stream();
}

/**
 * Finishes what close_stream() put off once the reader has taken the rest of the body, or is gone.
 * Not while a read is pending -- call_read_handler() still has the end of the body to deliver to
 * it -- and not from a nested call_read_handler(), which leaves that to the outer one.
 */
void NGHttp2Stream::finish_deferred_close()
{
   if (close_deferred && reading_finished() && !read_handler_ && !inside_call_read_handler_)
   {
      close_deferred = false;
      parent.close_stream(id);
   }
}

void NGHttp2Stream::maybe_close_stream()
{
   if (closed)
      mlogd("cleanup_stream: stream already closed");
   else if (writer)
      mlogd("cleanup_stream: still writing");
   else if (!eof_submitted || (!eof_received && !reader))
   {
      mlogi("cleanup_stream: not reading or writing any more, "
            "submitting RST with STREAM_CLOSED");

      //
      // One turn later, not here. This is reached from the write handler, which data_reader() runs
      // from inside nghttp2_conn_write() -- and nghttp2 drops the frame it is building for a stream
      // that has been reset in the meantime (NGHTTP2_STREAM_FLAG_RST_STREAM in
      // nghttp2_http_writer_write_data). That frame is the one carrying our own END_STREAM, so
      // resetting right here would replace the clean end of the response with a reset, and the peer
      // would report a body that stopped short. Version 1 queued its RST_STREAM behind the DATA
      // frame and needed no such care.
      //
      // Posting it is also harmless when the exchange completes by itself in the meantime: the
      // stream is closed by then, and this does nothing.
      //
      run_later(get_executor(), [self = weak_from_this()] {
         auto stream = self.lock();
         if (!stream || stream->closed || !stream->parent.conn)
            return;

         // this leads to the stream_close callback, eventually
         nghttp2_conn_shutdown_stream(stream->parent.conn, 0, stream->id, NGHTTP2_STREAM_CLOSED);
         stream->parent.start_write();
      });
   }
}

// =================================================================================================

template class NGHttp2Reader<client::Response::Impl>;
template class NGHttp2Reader<server::Request::Impl>;
template class NGHttp2Writer<client::Request::Impl>;
template class NGHttp2Writer<server::Response::Impl>;

} // namespace anyhttp::nghttp2
