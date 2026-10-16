//
// QuicStream: one QUIC stream, shared by the server and the client and by whatever protocol runs
// on top. See anyhttp/quic/stream.hpp for the model.
//
#include "anyhttp/quic/stream.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/quic/common.hpp"
#include "anyhttp/quic/session.hpp"

#include <boost/system/detail/errc.hpp>

#include <algorithm>
#include <format>
#include <utility>

using namespace boost::asio;

namespace anyhttp::quic
{

// =================================================================================================

QuicStream::QuicStream(QuicSession& s, int64_t stream_id, WriteMode mode)
   : id(stream_id), session(s), write_mode(mode)
{
   log_prefix_ = std::format("{}.{}", session.log_prefix(), id);
   mlogd("\x1b[1;33mStream: ctor\x1b[0m");
}

QuicStream::~QuicStream()
{
   detach_all();
   mlogd("\x1b[33mStream: dtor... done\x1b[0m");
}

void QuicStream::detach_all()
{
   //
   // A reader or writer adapter (owned by the user-visible handle) can outlive this stream, e.g.
   // when the session tears down its streams while a suspended coroutine still holds one. Detach
   // them so their destructors don't dereference a freed stream.
   //
   if (reader)
      std::exchange(reader, nullptr)->detach();
   if (writer)
      std::exchange(writer, nullptr)->detach();
   if (read_handler)
      swap_and_invoke(read_handler, make_error_code(errc::connection_reset), 0);
   if (write_active && write_handler)
      swap_and_invoke(write_handler, make_error_code(errc::connection_reset));
}

Executor QuicStream::get_executor() const noexcept { return session.get_executor(); }

// =================================================================================================
// Incoming body
// =================================================================================================

void QuicStream::on_data_chunk(const uint8_t* data, size_t len)
{
   if (len == 0)
      return;

   //
   // The stack hands us a view into the packet it is parsing, valid only until this callback
   // returns. Offer it to a waiting reader as it stands before copying it anywhere: a handler
   // that keeps a read outstanding -- the usual shape -- takes the bytes with a single copy, and
   // the vector that would otherwise carry them (a malloc, a copy in, a copy out and a free, per
   // QUIC packet, so about fifty of each per 64k of body) is never created at all. Only what the
   // reader could not take is parked for later.
   //
   auto self = shared_from_this(); // a resumed reader may drop the last reference to this stream
   incoming = asio::const_buffer{data, len};
   call_read_handler();

   if (incoming.size() > 0)
   {
      auto* rest = static_cast<const uint8_t*>(incoming.data());
      pending_read.emplace_back(rest, rest + incoming.size());
      if (read_head.size() == 0)
         read_head = asio::buffer(pending_read.front());
      incoming = {};
   }
}

void QuicStream::on_eof()
{
   eof_received = true;
   call_read_handler();
}

void QuicStream::call_read_handler()
{
   //
   // swap_and_invoke() below may resume a user coroutine that calls async_read_some() again
   // before returning, which re-enters this function. Letting that nested call do real work would
   // recurse once per buffered chunk -- with enough data queued up (e.g. after a large backlog
   // drains), that blows the C++ stack. Instead, the nested call just re-arms read_handler and
   // returns; the outer call's loop below picks it up and keeps going without growing the stack.
   //
   if (!read_handler || call_read_handler_active)
      return;

   call_read_handler_active = true;

   //
   // The loop below may resume a coroutine that drops the last owning reference to this stream
   // (e.g. the Response gets destroyed once EOF is delivered) -- or to the whole Session, when
   // that coroutine was the last user of the connection. Keep both alive until this function
   // returns: consume_stream() at the bottom dereferences the session, so outliving the stream
   // alone is not enough.
   //
   auto self = shared_from_this();
   auto session_guard = session.shared_from_this();

   size_t consumed = 0;
   while (read_handler)
   {
      if (read_head.size() > 0 || incoming.size() > 0)
      {
         //
         // Fill the caller's buffer from as many queued chunks as it takes, rather than stopping
         // at the end of the first one. Each chunk is what arrived in a single QUIC packet -- a
         // little over a kilobyte -- so handing them out one per read turns a 64k body into ~48
         // reads, and a handler that answers every read with a write (an echo) pays a full round
         // trip for each of them when the write only completes on acknowledgement (see
         // WriteMode::ZeroCopy).
         //
         auto dest = read_handler_buffer;
         size_t copied = 0;
         while (dest.size() > 0 && read_head.size() > 0)
         {
            auto n = asio::buffer_copy(dest, read_head);
            dest += n;
            read_head += n;
            copied += n;
            if (read_head.size() == 0)
            {
               pending_read.pop_front();
               read_head =
                  pending_read.empty() ? asio::const_buffer{} : asio::buffer(pending_read.front());
            }
         }

         //
         // ... and last from the chunk being delivered right now, which on_data_chunk() offers
         // through `incoming` instead of parking it in a vector of its own first. Queued chunks
         // go first: they arrived earlier.
         //
         if (dest.size() > 0 && incoming.size() > 0)
         {
            auto n = asio::buffer_copy(dest, incoming);
            incoming += n;
            copied += n;
         }

         consumed += copied;
         swap_and_invoke(read_handler, error_code{}, copied);
         continue;
      }

      if (eof_received)
      {
         // the end of the body, reported the way ASIO reports it everywhere else
         swap_and_invoke(read_handler, errors::eof, 0);
         continue;
      }

      if (closed || read_aborted)
      {
         //
         // The stream died before the body was complete, and this read was issued after that --
         // there is nothing left that could ever complete it, so report the truncation now rather
         // than leaving it pending forever.
         //
         swap_and_invoke(read_handler, errors::partial_message, 0);
         continue;
      }

      break;
   }
   call_read_handler_active = false;

   //
   // Grant the peer more send credit only for what was actually delivered to the app -- see
   // QuicSession::consume_stream() for why this must not happen any earlier.
   //
   session.consume_stream(id, consumed);
}

// =================================================================================================
// Outgoing body
// =================================================================================================

void QuicStream::start_write(WriteHandler&& handler, asio::const_buffer buffer, bool eof)
{
   auto n = asio::buffer_size(buffer);
   mlogd("start_write: n={} eof={}", n, eof);

   auto complete_immediately = [&](error_code ec) { //
      anyhttp::complete_immediately(std::move(handler), get_executor(), ec);
   };

   //
   // The protocol-independent entry ladder, in the order the Writer contract in common.hpp
   // prescribes: a zero-length non-EOF write is a free no-op wherever the body stands; after the
   // body has been ended, data has no body left to belong to -- through either entry point --
   // while a bare re-end is answered by how far the end has actually gotten.
   //
   if (n == 0 && !eof)
   {
      complete_immediately(error_code{});
      return;
   }

   //
   // Once accepted, the caller's intent to end the body is final: this is what tells
   // delete_writer() the body ended where it was meant to, so it need not reset the stream. An
   // earlier cancellation just makes for a shorter body than planned -- legitimate here, and a
   // declared content-length is still enforced by the peer. (Cancelling an EOF write whose FIN
   // was never handed over rolls the intent back, see bind_write_cancellation() -- such a write
   // does not land here again.)
   //
   if (eof_submitted)
   {
      if (n > 0)
      {
         mloge("start_write: body has already been ended");
         complete_immediately(make_error_code(errc::broken_pipe));
         return;
      }

      //
      // A bare re-end of an already-ended body. If that FIN is still pending on a live stream
      // (its handler was detached by cancellation), adopt this handler so it completes when the
      // FIN actually goes out. If the FIN was handed over, there is nothing left to do. And if
      // the stream died with the FIN still owed, it will never go out -- that is a reset, not a
      // harmless no-op.
      //
      if (write_active && write_is_eof && !closed && !write_aborted)
      {
         assert(!write_handler); // only a detached FIN may be adopted, never a live handler
         mlogd("start_write: FIN already pending, adopting handler");
         bind_write_cancellation(handler, write_token);
         write_handler = std::move(handler);
         return;
      }

      const bool dead = closed || write_aborted;
      complete_immediately(dead && !fin_offered ? make_error_code(errc::connection_reset)
                                                : error_code{});
      return;
   }

   if (closed || write_aborted)
   {
      mlogw("start_write: stream already closed");
      complete_immediately(make_error_code(errc::connection_reset));
      return;
   }

   //
   // Only one async_write() may be active at a time -- see the class comment above write_active.
   // The re-issued EOF handled above is not an exception to that: it adopts the FIN that is
   // already in flight instead of starting a write of its own, and has returned by now.
   //
   assert(!write_active);

   if (eof)
      eof_submitted = true;

   const uint64_t token = next_write_token++;
   bind_write_cancellation(handler, token);

   //
   // A new write starts where what is queued ends: in ZeroCopy, everything before has been
   // acknowledged or abandoned, in Staged, chunks of an earlier write may still be on their way,
   // and they come first.
   //
   write_active = true;
   write_source = buffer; // referenced, not copied -- see class comment above write_active
   write_base = tx_queued;
   write_copied = 0;
   write_is_eof = eof;
   write_token = token;
   write_handler = std::move(handler);
   if (write_mode == WriteMode::ZeroCopy)
      tx_queued = write_base + n;

   on_writable();
}

void QuicStream::bind_write_cancellation(WriteHandler& handler, uint64_t token)
{
   // Nothing to bind for a caller that passed no completion handler.
   if (!handler)
      return;

   on_cancel(handler, [this, token] {
      //
      // Cancellation completes the write immediately, without waiting for what it would normally
      // complete on -- see below for what that costs in either write mode.
      //
      if (write_token != token || !write_handler)
         return; // already completed naturally before the cancellation was delivered

      if (write_is_eof && asio::buffer_size(write_source) == 0)
      {
         //
         // A bare end-of-body carries no memory of the caller's, and a FIN cannot be un-sent --
         // it may just still be waiting for flow control credit. Detach the handler but leave
         // the write active so it still goes out: abandoning it would leave the stream half-open
         // forever, with the peer waiting for an end that never comes.
         //
         // A *data-carrying* EOF write does not get this treatment: its buffer belongs to the
         // caller, who is free to destroy it the moment the handler runs, so it must be cancelled
         // exactly like any other data write below -- keeping it active would leave the stack
         // pointing into freed memory.
         //
         mlogd("async_write: \x1b[1;31mcancelled\x1b[0m, FIN still pending");
         complete_later(std::move(write_handler), get_executor(), errors::canceled);
         return;
      }
      mlogd("async_write: \x1b[1;31mcancelled\x1b[0m");

      if (write_mode == WriteMode::ZeroCopy)
      {
         //
         // The handler runs now, and the caller is free to destroy its buffer the moment it does
         // -- but the stack points straight into that buffer (see WriteMode::ZeroCopy), so
         // whatever it holds on to and is not acknowledged yet has to stop being referenced
         // first. Only a reset can guarantee that: RESET_STREAM makes ngtcp2 drop the stream's
         // queued data and keeps it from reclaiming in-flight bytes for retransmission. That
         // costs nothing in expressiveness -- a body cut short mid-write is truncated, which is
         // exactly what delete_writer() resets the stream for as well.
         //
         // A write that never got to hand over a byte, or whose bytes are all acknowledged
         // already, leaves nothing behind and lets the stream carry on unharmed: what was not
         // handed over is simply no longer there to send.
         //
         if (tx_offered > tx_acked && !closed && !write_aborted)
         {
            mlogw("async_write: cancelled with {} bytes unacknowledged, resetting stream",
                  tx_offered - tx_acked);
            reset_writing();
         }
         tx_queued = tx_offered;
      }

      //
      // Staged mode never lets the stack see the caller's buffer, only our own copies of it, so
      // the un-copied remainder can simply be abandoned -- same as HTTP/2, where cancelling drops
      // the unsent remainder of write_buffer. The chunks already copied still go out. The stream
      // stays healthy, and the caller may issue a fresh async_write() as soon as the handler
      // fires.
      //
      // If this write was to end the body but its FIN was never handed over, the body is not
      // ended after all: roll the intent back, so that a later async_write_eof() takes the normal
      // path and actually sends the FIN of the (now shorter) body, instead of completing as a
      // no-op while the peer waits forever. In ZeroCopy mode the intent is rolled back
      // unconditionally -- there, a FIN that was handed over went with unacknowledged bytes,
      // which is exactly the case that just reset the stream, so nothing of it survives either
      // way. Only a Staged write whose FIN was handed over with its final chunk keeps its end
      // standing: that FIN rides out with the chunks all by itself.
      //
      if (write_is_eof && (write_mode == WriteMode::ZeroCopy || !fin_offered))
         eof_submitted = false;

      write_active = false;
      write_source = {};
      // make sure to post this -- otherwise "MAIN COROUTINE DID NOT COMPLETE" happens
      complete_later(std::move(write_handler), get_executor(), errors::canceled);
   });
}

// -------------------------------------------------------------------------------------------------

size_t QuicStream::pending_data(uint64_t from, std::span<ngtcp2_vec> vec, bool& fin)
{
   fin = false;
   if (vec.empty() || closed || write_aborted)
      return 0;

   const size_t total = asio::buffer_size(write_source);
   const auto* source = static_cast<const uint8_t*>(write_source.data());

   //
   // Copying is paced by what goes out: the next slice of the caller's buffer is carved off only
   // once everything queued before it is in packets, rather than copying a huge caller buffer in
   // one go.
   //
   if (write_mode == WriteMode::Staged && write_active && tx_sent >= tx_queued &&
       write_copied < total)
   {
      const size_t take = std::min(total - write_copied, kWriteChunkSize);
      chunks.push_back({tx_queued, {source + write_copied, source + write_copied + take}});
      write_copied += take;
      tx_queued += take;
   }

   size_t n = 0;
   uint64_t end = from;
   if (write_mode == WriteMode::ZeroCopy)
   {
      //
      // Hand out what is left of the caller's buffer, by reference and in one go: a vec is just a
      // pointer and a length, so there is nothing to be gained from slicing it up.
      //
      if (write_active && from < tx_queued)
      {
         vec[n++] = {const_cast<uint8_t*>(source + (from - write_base)), tx_queued - from};
         end = tx_queued;
      }
   }
   else
   {
      for (auto& chunk : chunks)
      {
         const uint64_t chunk_end = chunk.offset + chunk.data.size();
         if (chunk_end <= from)
            continue;
         if (n == vec.size())
            break;
         const size_t skip = from > chunk.offset ? from - chunk.offset : 0;
         vec[n++] = {chunk.data.data() + skip, chunk.data.size() - skip};
         end = chunk_end;
      }
   }

   //
   // The one place that decides whether the data about to be handed out ends the body, so the FIN
   // can ride along with it instead of costing a call -- and a QUIC packet -- of its own. Note
   // that handing out the FIN does not complete a write that carries data: in ZeroCopy it still
   // completes on acknowledgement, in Staged once all of it has gone into packets.
   //
   const bool all_queued = write_mode == WriteMode::ZeroCopy || write_copied == total;
   fin = write_active && write_is_eof && all_queued && end >= tx_queued;
   return n;
}

void QuicStream::on_offered(uint64_t end, bool fin)
{
   tx_offered = std::max(tx_offered, std::min(end, tx_queued));
   if (!fin)
      return;

   fin_offered = true;

   //
   // A bare end-of-body completes as soon as the stack has taken the FIN: unlike body data, it
   // carries no memory of the caller's that we would have to keep alive until it is
   // acknowledged.
   //
   if (write_active && asio::buffer_size(write_source) == 0)
      finish_active_write();
}

void QuicStream::on_sent(uint64_t end)
{
   if (end <= tx_sent)
      return;
   tx_sent = std::min(end, tx_offered);

   if (write_mode != WriteMode::Staged || !write_active || tx_sent < tx_queued)
      return;
   if (asio::buffer_size(write_source) == 0)
      return; // a bare end-of-body completes when the FIN is handed over, see on_offered()

   //
   // The write is done once all of its chunks have gone into packets and there is no more of
   // write_source left to carve. Otherwise there is something new to hand out now -- and the
   // scheduler may have been told there was nothing, while the last chunk was still on its way.
   // Without telling it otherwise, any single async_write() larger than kWriteChunkSize would
   // stall here forever, with the body truncated and no FIN.
   //
   if (write_copied == asio::buffer_size(write_source))
      finish_active_write();
   else
      on_writable();
}

void QuicStream::on_acked(uint64_t end)
{
   //
   // Acknowledgements come in order and without overlap, and never for what the stack was not
   // given. Clamp defensively anyway -- an accounting mismatch should complete the write early,
   // not run past the end of the buffer.
   //
   end = std::min(end, tx_offered);
   if (end <= tx_acked)
      return;
   tx_acked = end;
   mlogd("on_acked: {} bytes acknowledged", tx_acked);

   // a chunk the peer has is of no more use: ngtcp2 never retransmits acknowledged data
   while (!chunks.empty() && chunks.front().offset + chunks.front().data.size() <= tx_acked)
      chunks.pop_front();

   //
   // The only notification that the peer is done with bytes we handed out by reference, and
   // hence that the caller's buffer may be released -- see WriteMode::ZeroCopy.
   //
   if (write_mode == WriteMode::ZeroCopy && write_active && asio::buffer_size(write_source) > 0 &&
       tx_acked >= write_base + asio::buffer_size(write_source))
      finish_active_write();
}

void QuicStream::finish_active_write()
{
   assert(write_active);

   //
   // Invoking the handler hands the caller's buffer back to it, so this must only ever run when
   // nothing points into it any more: every byte acknowledged (ZeroCopy), copied out (Staged), or
   // no bytes at all (the EOF marker).
   //
   write_source = {};
   auto handler = std::move(write_handler);
   write_active = false;

   if (!handler)
      return;

   //
   // Every path here runs inside a stack callback and hence inside an ngtcp2 call: the EOF marker
   // and a staged chunk complete while ngtcp2 is packing a packet, acknowledged data while it is
   // parsing one. Resuming the application there would let it call back into ngtcp2 from inside
   // ngtcp2 (destroying the session writes a CONNECTION_CLOSE, say), which at best confuses the
   // write pass this is nested in and at worst trips ngtcp2's own "time must not go backwards"
   // assertion. Post instead -- one hop, on a path that is not latency critical.
   //
   run_later(get_executor(), [self = shared_from_this(), handler = std::move(handler)]() mutable {
      std::move(handler)(error_code{});
   });
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void QuicStream::fail(error_code ec)
{
   //
   // The handlers below may run synchronously and drop the last owning reference to this stream
   // (e.g. the coroutine they resume destroys its Request/Response), reentrantly erasing it from
   // the session. Keep it alive until fail() itself returns.
   //
   auto self = shared_from_this();
   closed = true;

   if (read_handler)
   {
      //
      // The stream died before the body was complete. What the reader cares about is that it will
      // never see the rest of it, not which QUIC error code carried that news -- report the
      // truncation, matching what the HTTP/2 side delivers for a stream closing early.
      //
      auto read_ec = (ec && !eof_received) ? errors::partial_message : ec;
      swap_and_invoke(read_handler, read_ec, 0);
   }

   on_failed(ec);

   //
   // A write waiting for its data to be acknowledged will never see those acknowledgements now:
   // ngtcp2 drops whatever of this stream is still in flight. It also stops touching the caller's
   // buffer, which is all the wait was ever for, so complete the write -- as failed, because the
   // body did not make it -- instead of leaving it pending forever. This must happen even when
   // the handler was detached by cancellation (a pending FIN): leaving such a write marked
   // active would let start_write() adopt a fresh handler onto a stream that is never written
   // again.
   //
   if (write_active)
   {
      write_active = false;
      write_source = {};
      if (write_handler)
         swap_and_invoke(write_handler, ec ? ec : make_error_code(errc::connection_reset));
   }

   maybe_close();
}

void QuicStream::delete_reader()
{
   auto self = shared_from_this(); // see delete_writer()
   pending_read.clear();
   read_head = {};
   incoming = {};

   //
   // The application dropped its side of the exchange without reading the body to its end (e.g.
   // not_found(), which never looks at the request). Stream-level flow control credit is only
   // granted as the application actually reads (see QuicSession::consume_stream()), so a peer
   // with more body to send would stall forever against a window that will now never reopen.
   // Tell it to stop instead: STOP_SENDING half-closes only our read direction, leaving whatever
   // we are still writing to flow normally -- HTTP/2 has to submit a full RST_STREAM here for
   // lack of a half-close.
   //
   if (!eof_received && !closed && !read_aborted)
   {
      mlogd("delete_reader: body not read to end, sending STOP_SENDING");
      session.stop_reading(id, stop_sending_error_code());
   }

   maybe_close();
}

void QuicStream::delete_writer()
{
   //
   // The teardown paths below can run handlers that drop the last reference to this stream,
   // erasing it from the session -- keep it alive until this function returns.
   //
   auto self = shared_from_this();

   //
   // Nothing to finalize on a stream ngtcp2 has already torn down (peer reset it, or we did),
   // or whose sending side is: there is nothing left to reset.
   //
   if (closed || write_aborted)
   {
      mlogd("delete_writer: stream already closed");
      maybe_close();
      return;
   }

   if (!eof_submitted)
   {
      //
      // The writer was dropped without ever ending the body (write_eof()), so wherever it stopped
      // is not where it was meant to stop. Sending a FIN here would present that partial body to
      // the peer as a complete one -- reset our side instead. The peer's side is the reader's
      // business.
      //
      mlogw("delete_writer: body never ended, resetting stream");
      reset_writing();
      abort_writing(make_error_code(errc::connection_reset));
   }

   maybe_close();
}

void QuicStream::reset_writing()
{
   session.reset_writing(id, reset_error_code());
   write_aborted = true;
}

void QuicStream::abort_reading()
{
   //
   // A body that has arrived in full is not affected: ngtcp2 reports no reset after the FIN, and
   // what of it is buffered is still the reader's.
   //
   if (eof_received || read_aborted)
      return;

   read_aborted = true;
   pending_read.clear();
   read_head = {};
   incoming = {};
   call_read_handler(); // reports the truncation to a read waiting for more
}

void QuicStream::abort_writing(error_code ec)
{
   write_aborted = true;
   if (!write_active)
      return;

   //
   // ngtcp2 has dropped what it had of the stream and won't read from it again, so the caller's
   // buffer is free -- in ZeroCopy as much as in Staged mode. Posted, as this is reached from
   // inside ngtcp2.
   //
   write_active = false;
   write_source = {};
   if (write_handler)
      complete_later(std::move(write_handler), get_executor(), ec);
}

void QuicStream::maybe_close()
{
   if (reader || writer)
      return;
   if (!closed)
      return;
   session.erase_stream(id);
}

// =================================================================================================

} // namespace anyhttp::quic
