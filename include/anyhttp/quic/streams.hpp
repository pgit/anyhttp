#pragma once

//
// Plain QUIC streams (anyhttp/quic.hpp) on a QUIC connection: the QuicApplication that hands the
// streams straight to their Readers and Writers, with no protocol in between.
//

#include "anyhttp/quic.hpp"
#include "anyhttp/quic/application.hpp"
#include "anyhttp/quic/stream.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <tuple>

namespace anyhttp::quic
{

class StreamApplication;

// =================================================================================================

//
// A QuicStream as plain streams have it: every byte is body, so body offsets are stream offsets,
// and each direction can end -- or be aborted -- on its own.
//
class PlainStream final : public QuicStream
{
public:
   PlainStream(StreamApplication& application, int64_t id);
   ~PlainStream() override;

   StreamApplication& application;

   bool scheduled = false; // in the application's write queue
   bool blocked = false; // waiting for the peer to extend its flow control window

protected:
   void on_writable() override;
   uint64_t reset_error_code() const noexcept override;
   uint64_t stop_sending_error_code() const noexcept override;
};

// =================================================================================================

//
// Plain streams on a QUIC connection, the same on both sides. A stream the peer opens waits in a
// queue until accept_stream() takes it, one we open is there right away. Each stream reports what
// it wants to send to the round-robin write queue, from which write_pkt() takes one stream per
// packet; a stream out of flow control credit leaves the queue until the peer extends it.
//
// Flow control: stream credit is granted as a stream's Reader reads (QuicStream), connection
// credit as soon as the data arrives -- a stream nobody reads holds up only itself.
//
class StreamApplication final : public QuicApplication
{
public:
   //
   // `handler`, if given, gets the connection once it is up: that is the server's side. The client
   // takes it from quic::connect().
   //
   StreamApplication(QuicSession& session, Config config, ConnectionHandler handler);
   ~StreamApplication() override;

   const Config& config() const noexcept { return config_; }
   const std::string& log_prefix() const noexcept;

   //
   // QuicApplication
   //
   void fill_transport_params(ngtcp2_transport_params& params) override;
   int on_ready() override;
   std::shared_ptr<QuicStream> make_stream(int64_t id) override;
   void on_closed(error_code ec) override;
   int stream_open(int64_t id) override;
   int recv_stream_data(int64_t id, std::span<const uint8_t> data, bool fin) override;
   int acked_stream_data(int64_t id, uint64_t offset, uint64_t len) override;
   int stream_close(int64_t id, uint32_t flags, uint64_t app_error_code) override;
   int stream_reset(int64_t id, uint64_t app_error_code) override;
   int stream_stop_sending(int64_t id, uint64_t app_error_code) override;
   int recv_stop_sending(int64_t id, uint64_t app_error_code) override;
   int extend_max_stream_data(int64_t id) override;
   ngtcp2_ssize next_stream_data(int64_t& id, bool& fin, std::span<ngtcp2_vec> vec) override;
   int on_stream_data_written(int64_t id, size_t n, bool fin) override;
   void on_stream_blocked(int64_t id) override;
   void on_stream_shut_wr(int64_t id) override;

   //
   // What Connection offers.
   //
   std::tuple<error_code, Stream> open_stream();
   void async_accept_stream(AcceptStreamHandler&& handler);

   /// Puts `stream` at the end of the write queue, unless it is in there already.
   void schedule(PlainStream& stream, bool wake = true);

private:
   std::shared_ptr<PlainStream> find_stream(int64_t id);

   /// Whether the peer opened stream `id`.
   bool remote_stream(int64_t id) const noexcept;

   /// The stream `id` the peer opened, coming into being: queued for accept_stream().
   std::shared_ptr<PlainStream> open_remote_stream(int64_t id);

   /// Hands the oldest stream waiting to the accept_stream() waiting, if there are both.
   void deliver_accepted();

   /// Removes `id` from the front of the write queue, where write_pkt() took it from.
   void unschedule_front(PlainStream& stream);

private:
   Config config_;
   ConnectionHandler handler_;

   std::deque<std::shared_ptr<PlainStream>> accept_queue_;
   AcceptStreamHandler accept_handler_;
   error_code closed_ec_; // why the connection is over, once it is

   std::deque<int64_t> write_queue_;
};

// =================================================================================================

//
// What a Connection handle holds: the session, which owns the application.
//
class Connection::Impl
{
public:
   Impl(std::shared_ptr<QuicSession> session, StreamApplication& application) noexcept
      : session(std::move(session)), application(application)
   {
   }
   ~Impl();

   Impl(const Impl&) = delete;
   Impl& operator=(const Impl&) = delete;

   std::shared_ptr<QuicSession> session;
   StreamApplication& application;
};

// =================================================================================================

} // namespace anyhttp::quic
