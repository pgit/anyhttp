#pragma once

#include "anyhttp/common.hpp"
#include "anyhttp/reader_impl.hpp"
#include "anyhttp/writer_impl.hpp"

#include <boost/asio/buffer.hpp>

#include <ngtcp2/ngtcp2.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace anyhttp::quic
{

class QuicSession;

// =================================================================================================

//
// How a stream hands the caller's async_write() buffer down to the QUIC stack.
//
enum class WriteMode
{
   //
   // Point straight into the caller's buffer: the body is never copied on its way to ngtcp2,
   // however large it is -- the mmap()ed file of serve_file() travels from the page cache into
   // QUIC packets without an intermediate byte. What that costs is *when* the write completes:
   // ngtcp2 keeps pointing into that memory for as long as the bytes may still have to be
   // retransmitted, so the handler -- which releases the buffer -- can only run once they are
   // acknowledged. Cancelling a write with unacknowledged bytes therefore has to reset the stream,
   // there being no way to un-offer memory ngtcp2 may still read from.
   //
   ZeroCopy,

   //
   // Copy through bounded, stream-owned chunks (<= kWriteChunkSize each), the next one carved off
   // the caller's buffer once the one before has gone into packets. That costs a copy but
   // completes a write as soon as the bytes are handed over, and makes cancellation
   // instantaneous: nothing points into the caller's buffer, so the un-copied remainder is simply
   // abandoned and the *stream survives* -- which is what lets a cancelled write be followed by
   // another one on the same stream. A chunk lives until the peer has acknowledged it.
   //
   Staged
};

// =================================================================================================

//
// One QUIC stream that can be read from and written to: the byte stream itself, whatever it
// carries. The session keeps track of it by ID and fails it when the connection goes away; the
// application on top (see QuicApplication::make_stream()) feeds it what arrives and asks it what
// to send, and the Reader/Writer adapters of the protocol hand it to the user.
//
// Everything outgoing is counted in bytes of the *body* -- what the user wrote, from its start.
// For a plain stream that is the stream offset; HTTP/3 frames the body on its way through
// nghttp3, so there the two differ by the framing, and Http3Stream translates.
//
class QuicStream : public std::enable_shared_from_this<QuicStream>
{
public:
   QuicStream(QuicSession& session, int64_t id, WriteMode write_mode);
   virtual ~QuicStream();

   QuicStream(const QuicStream&) = delete;
   QuicStream& operator=(const QuicStream&) = delete;

   int64_t id;
   QuicSession& session;
   std::string log_prefix_;

   Executor get_executor() const noexcept;
   const std::string& log_prefix() const noexcept { return log_prefix_; }

   //
   // Incoming body. What the application delivers is parked here only if no reader was waiting
   // for it.
   //
   std::deque<std::vector<uint8_t>> pending_read;
   asio::const_buffer read_head; // view of pending_read.front() not yet delivered
   asio::const_buffer incoming; // chunk on_data_chunk() is delivering, not yet taken
   bool eof_received = false;
   ReadSomeHandler read_handler;
   asio::mutable_buffer read_handler_buffer;
   bool call_read_handler_active = false; // re-entrancy guard, see call_read_handler()

   //
   // Outgoing body. Only one async_write() may be active at a time -- callers must wait for its
   // handler before issuing another (same contract as e.g. Beast) -- so this is flat per-stream
   // state rather than a queue of pending writes.
   //
   // write_source is the caller's buffer, referenced, not copied, the way asio::async_write
   // generally requires: it must stay valid until write_handler fires. What completes the write
   // depends on write_mode: all of it acknowledged in ZeroCopy, all of it copied out and handed
   // over in Staged. See WriteMode.
   //
   const WriteMode write_mode;
   bool write_active = false;
   asio::const_buffer write_source;
   //
   // Whether the active write ends the body. The FIN then goes out with the write's last bytes
   // -- one QUIC STREAM frame carrying both the tail of the body and the FIN -- rather than
   // needing a write of its own.
   //
   bool write_is_eof = false;
   WriteHandler write_handler;
   uint64_t write_token = 0;
   uint64_t next_write_token = 1;
   bool eof_submitted = false; // user ended the body via async_write_eof()
   bool fin_offered = false; // ... and the FIN has been handed over; between the two lies a
                             // cancelled async_write_eof(), which rolls eof_submitted back when
                             // the FIN is still owed

   //
   // Positions in the outgoing body. Each is the end of a prefix of it, and each one is at most
   // the one before: what there is to send, what the stack holds on to, what went into packets,
   // and what the peer acknowledged. "Held on to" is what the stack may still read from until it
   // is acknowledged: nghttp3 keeps what it is offered, ngtcp2 only what it wrote.
   //
   uint64_t write_base = 0; // where write_source begins
   size_t write_copied = 0; // Staged: how much of write_source went into chunks
   uint64_t tx_queued = 0;
   uint64_t tx_offered = 0;
   uint64_t tx_sent = 0;
   uint64_t tx_acked = 0;

   struct Chunk
   {
      uint64_t offset; // in the body
      std::vector<uint8_t> data;
   };
   std::deque<Chunk> chunks; // Staged: the copies in [tx_acked, tx_queued)

   //
   // Lifecycle.
   //
   Reader::Impl* reader = nullptr; // the reading adapter, while attached
   Writer::Impl* writer = nullptr; // the writing adapter, while attached
   bool closed = false; // both directions are over, ngtcp2 is done with the stream
   bool read_aborted = false; // the incoming body will not be complete, see abort_reading()
   bool write_aborted = false; // the outgoing body can't go on, see abort_writing()

   //
   // Data flow into user land (incoming body).
   //
   void on_data_chunk(const uint8_t* data, size_t len);
   void on_eof();
   void call_read_handler();

   /// True once the peer ended the body and every byte of it has been delivered to the reader.
   bool reading_finished() const noexcept
   {
      return eof_received && read_head.size() == 0 && incoming.size() == 0;
   }

   //
   // Data flow from user land to the stack (outgoing body).
   //
   void start_write(WriteHandler&& handler, asio::const_buffer buffer, bool eof);

   //
   // Points `vec` at what there is to send from body offset `from` on, and returns how many
   // entries that took. `fin` tells whether the end of the body comes with the last of them --
   // which can be with none at all. In Staged mode, this is also where the next chunk is copied,
   // once everything before it has gone into packets.
   //
   size_t pending_data(uint64_t from, std::span<ngtcp2_vec> vec, bool& fin);

   //
   // What the stack reports back, in body offsets: it holds on to everything up to `end` (and
   // the FIN, with `fin`), wrote it into packets, the peer acknowledged it.
   //
   void on_offered(uint64_t end, bool fin);
   void on_sent(uint64_t end);
   void on_acked(uint64_t end);

   //
   // Called when the stream dies before its exchange completed, and from either the reader's or
   // the writer's destructor.
   //
   virtual void fail(error_code ec);
   void delete_reader();
   virtual void delete_writer();
   void maybe_close();

   //
   // One direction ends early, for protocols that let the two go separate ways (plain streams;
   // HTTP/3 aborts the whole exchange instead). abort_reading(): the peer reset its side, or we
   // stopped reading -- what is buffered is dropped, and reads report partial_message.
   // abort_writing(): the peer asked us to stop (ngtcp2 has answered with RESET_STREAM), or we
   // reset our side -- a write in progress completes with `ec`, every later one with
   // connection_reset.
   //
   void abort_reading();
   void abort_writing(error_code ec);

protected:
   //
   // Detaches the reader and writer adapters and fails what they have pending. The destructor of
   // the most derived stream calls this first: an adapter latches what it needs from the stream
   // on detach(), and that includes what only the derived stream has.
   //
   void detach_all();

   //
   // What the protocol on top decides.
   //
   /// The stream failed or closed early; fail whatever else the protocol has pending.
   virtual void on_failed(error_code ec) { (void)ec; }
   /// There is something new to send: tell the scheduler (and wake the write loop).
   virtual void on_writable() = 0;
   /// Cuts the outgoing body short with RESET_STREAM and marks it dead, completing nothing: what
   /// a cancelled zero-copy write and a writer gone before the end come down to. HTTP/3 resets
   /// the whole stream instead.
   virtual void reset_writing();
   /// The application error code for RESET_STREAM when a body is cut short.
   virtual uint64_t reset_error_code() const noexcept = 0;
   /// The application error code for STOP_SENDING when the reader goes away early.
   virtual uint64_t stop_sending_error_code() const noexcept = 0;

private:
   void bind_write_cancellation(WriteHandler& handler, uint64_t token); // arms cancellation
   void finish_active_write(); // completes the active write and releases the caller's buffer
};

// =================================================================================================
// QuicReader / QuicWriter: adapters plugging a QuicStream into the anyhttp Reader/Writer
// interfaces, for both roles and every protocol. HTTP/3 derives from them what it adds on top --
// the fields, the content length, the request or status line.
// =================================================================================================

template <typename Interface, typename Stream = QuicStream>
class QuicReader : public Interface
{
public:
   explicit QuicReader(Stream& s) : stream(&s), stream_id(s.id), executor(s.get_executor())
   {
      s.reader = this;
   }
   ~QuicReader() override
   {
      if (stream)
      {
         stream->reader = nullptr;
         stream->delete_reader();
      }
   }

   Executor get_executor() const noexcept override { return executor; }

   std::optional<size_t> content_length() const noexcept override { return std::nullopt; }

   void async_read_some(asio::mutable_buffer buffer, ReadSomeHandler&& handler) override
   {
      //
      // An empty buffer is not a request to read anything: complete right away, without looking
      // at whether the body has ended or the stream is even still there -- as ASIO does for a
      // zero-length read.
      //
      if (asio::buffer_size(buffer) == 0)
      {
         complete_immediately(std::move(handler), executor, error_code{}, size_t{0});
         return;
      }

      //
      // The stream is gone; detach() latched how the body stood at that point, so a cleanly
      // finished body keeps reporting eof (as the Reader contract requires) and a truncated one
      // keeps reporting partial_message.
      //
      if (!stream)
      {
         complete_immediately(std::move(handler), executor, detached_ec, size_t{0});
         return;
      }

      on_cancel(handler, [this] {
         if (stream && stream->read_handler)
            complete_later(std::move(stream->read_handler), stream->get_executor(),
                           errors::canceled, size_t{0});
      });

      assert(!stream->read_handler);
      stream->read_handler = std::move(handler);
      stream->read_handler_buffer = buffer;
      stream->call_read_handler();
   }

   void detach() override
   {
      //
      // The stream is going away first (session teardown outliving this exchange). Remember how
      // the body stood, so that reads issued from now on keep answering per the Reader contract.
      //
      assert(stream);
      detached_ec = stream->reading_finished() ? errors::eof : errors::partial_message;
      detached_log_prefix = stream->log_prefix();
      stream = nullptr;
   }

   std::string log_prefix() const override
   {
      return stream ? stream->log_prefix() : detached_log_prefix;
   }

   Stream* stream;
   int64_t stream_id;
   Executor executor; // kept as a copy so a detached reader can still complete
   std::string detached_log_prefix; // latched by detach()

   /// What a read past detach() reports: eof for a body read to its clean end, else truncation.
   error_code detached_ec{errors::partial_message};
};

// -------------------------------------------------------------------------------------------------

template <typename Interface, typename Stream = QuicStream>
class QuicWriter : public Interface
{
public:
   explicit QuicWriter(Stream& s) : stream(&s), stream_id(s.id), executor(s.get_executor())
   {
      s.writer = this;
   }
   ~QuicWriter() override
   {
      if (stream)
      {
         stream->writer = nullptr;
         stream->delete_writer();
      }
   }

   Executor get_executor() const noexcept override { return executor; }

   void content_length(std::optional<size_t>) override {}

   void async_write(WriteHandler&& handler, asio::const_buffer buffer, bool eof) override
   {
      if (stream)
      {
         // everything -- including a write against a stream ngtcp2 has already torn down -- is
         // start_write()'s to decide, so that ending a body twice and writing past its end are
         // answered the same way whatever became of the stream since
         stream->start_write(std::move(handler), buffer, eof);
         return;
      }

      //
      // The stream itself is gone, but the entry ladder of the Writer contract still applies,
      // answered from the state detach() latched: an empty non-EOF write stays a free no-op, a
      // body that was cleanly ended keeps answering as such -- bare re-end idempotent, data
      // broken_pipe -- and only a stream that vanished mid-body is a connection error.
      //
      const bool empty = asio::buffer_size(buffer) == 0;
      error_code ec;
      if (empty && !eof)
         ec = {};
      else if (detached_body_ended)
         ec = empty ? error_code{} : make_error_code(errc::broken_pipe);
      else
         ec = make_error_code(errc::connection_reset);
      complete_immediately(std::move(handler), executor, ec);
   }

   void detach() override
   {
      // remember whether the body was cleanly ended -- intent accepted *and* the FIN handed
      // over -- so writes issued after this still answer per the Writer contract
      assert(stream);
      detached_body_ended = stream->eof_submitted && stream->fin_offered;
      detached_log_prefix = stream->log_prefix();
      stream = nullptr;
   }

   std::string log_prefix() const override
   {
      return stream ? stream->log_prefix() : detached_log_prefix;
   }

   Stream* stream;
   int64_t stream_id;
   Executor executor; // kept as a copy so a detached writer can still complete
   std::string detached_log_prefix; // latched by detach()
   bool detached_body_ended = false; // latched by detach(), see there
};

// =================================================================================================

} // namespace anyhttp::quic
