//
// The public handles of plain QUIC streams, see anyhttp/quic.hpp: Stream, Connection, connect().
//
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/quic.hpp"
#include "anyhttp/quic/client.hpp"
#include "anyhttp/quic/session.hpp"
#include "anyhttp/quic/streams.hpp"

#include <boost/system/detail/errc.hpp>

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace anyhttp::quic
{

// =================================================================================================
// Stream
// =================================================================================================

Stream::Stream() noexcept : Reader(nullptr), Writer(nullptr) {}

Stream::Stream(std::shared_ptr<Reader::Impl> reader, std::shared_ptr<Writer::Impl> writer) noexcept
   : Reader(std::move(reader)), Writer(std::move(writer))
{
}

Stream::Stream(Stream&& other) noexcept = default;
Stream& Stream::operator=(Stream&& other) noexcept = default;
Stream::~Stream() = default;

Stream::executor_type Stream::get_executor() const noexcept
{
   return Reader::operator bool() ? Reader::get_executor() : Writer::get_executor();
}

std::string Stream::log_prefix() const
{
   return Reader::operator bool() ? Reader::log_prefix() : Writer::log_prefix();
}

void Stream::reset() noexcept
{
   Reader::reset();
   Writer::reset();
}

namespace
{

using StreamReader = QuicReader<Reader::Impl>;
using StreamWriter = QuicWriter<Writer::Impl>;

} // namespace

Reader::Impl* Stream::reader_impl() const noexcept
{
   return Reader::operator bool() ? &Reader::pimpl() : nullptr;
}

int64_t Stream::id() const noexcept
{
   if (auto* reader = reader_impl())
      return static_cast<StreamReader*>(reader)->stream_id;
   if (Writer::operator bool())
      return static_cast<StreamWriter&>(Writer::pimpl()).stream_id;
   return -1;
}

namespace
{

//
// The stream behind a handle, through whichever half it still has. Null once the stream is gone.
//
QuicStream* stream_of(Reader::Impl* reader, Writer::Impl* writer)
{
   if (reader)
      if (auto* stream = static_cast<StreamReader*>(reader)->stream)
         return stream;
   if (writer)
      return static_cast<StreamWriter*>(writer)->stream;
   return nullptr;
}

} // namespace

void Stream::reset_stream(uint64_t code)
{
   auto* writer = Writer::operator bool() ? &Writer::pimpl() : nullptr;
   auto* stream = stream_of(reader_impl(), writer);
   if (!stream || stream->closed || stream->write_aborted)
      return;
   stream->session.reset_writing(stream->id, code);
   stream->abort_writing(errors::canceled);
}

void Stream::stop_sending(uint64_t code)
{
   auto* writer = Writer::operator bool() ? &Writer::pimpl() : nullptr;
   auto* stream = stream_of(reader_impl(), writer);
   if (!stream || stream->closed || stream->read_aborted || stream->eof_received)
      return;
   stream->session.stop_reading(stream->id, code);
   stream->abort_reading();
}

// =================================================================================================
// Connection
// =================================================================================================

Connection::Connection() noexcept = default;
Connection::Connection(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Connection::Connection(Connection&& other) noexcept = default;
Connection& Connection::operator=(Connection&& other) noexcept = default;
Connection::~Connection() = default;

Connection::executor_type Connection::get_executor() const noexcept
{
   return impl_ ? impl_->session->get_executor() : executor_type{};
}

std::string Connection::log_prefix() const
{
   return impl_ ? impl_->session->log_prefix() : std::string{};
}

void Connection::async_open_stream_any(OpenStreamHandler&& handler)
{
   if (!impl_)
   {
      std::move(handler)(errors::bad_descriptor, Stream{});
      return;
   }
   impl_->application.async_open_stream(std::move(handler));
}

IoTask<Stream> Connection::open_stream()
{
   co_return co_await initiate<OpenStream>([this](OpenStreamHandler handler) { //
      async_open_stream_any(std::move(handler));
   });
}

results<Stream> Connection::try_open_stream()
{
   if (!impl_)
      return {errors::bad_descriptor, Stream{}};
   return impl_->application.try_open_stream();
}

void Connection::async_accept_stream_any(AcceptStreamHandler&& handler)
{
   if (!impl_)
   {
      std::move(handler)(errors::bad_descriptor, Stream{});
      return;
   }
   impl_->application.async_accept_stream(std::move(handler));
}

IoTask<Stream> Connection::accept_stream()
{
   co_return co_await initiate<AcceptStream>([this](AcceptStreamHandler handler) { //
      async_accept_stream_any(std::move(handler));
   });
}

void Connection::close(uint64_t code) noexcept
{
   if (impl_)
      impl_->session->close_connection(code);
}

void Connection::reset() noexcept { impl_.reset(); }

// =================================================================================================
// connect()
// =================================================================================================

namespace
{

//
// What connect() makes of either the endpoints it is given or the ones it looks up.
//
template <typename Target>
IoTask<Connection> connect_to(Executor executor, Target target, std::string host, Config config)
{
   if (config.alpn.empty() || config.alpn.size() > 255)
      co_return std::tuple{make_error_code(errc::invalid_argument), Connection{}};

   try
   {
      ClientConfig client_config{.alpn = config.alpn,
                                 .host = std::move(host),
                                 .tls_ca_file = config.tls_ca_file,
                                 .tls_keylog_file = config.tls_keylog_file,
                                 .verify = config.tls_verify,
                                 .idle_timeout = config.idle_timeout};
      auto factory = [config](QuicSession& s) -> std::unique_ptr<QuicApplication> {
         return std::make_unique<StreamApplication>(s, config, nullptr);
      };

      std::shared_ptr<QuicSession> session;
      if constexpr (std::is_same_v<Target, std::vector<TcpEndpoint>>)
         session = co_await async_connect(std::move(executor), std::move(target),
                                          std::move(client_config), std::move(factory));
      else
      {
         auto host_to_resolve = client_config.host;
         session =
            co_await async_connect(std::move(executor), std::move(host_to_resolve),
                                   std::move(target), std::move(client_config), std::move(factory));
      }

      auto& application = static_cast<StreamApplication&>(*session->application());
      co_return std::tuple{error_code{},
                           Connection{std::make_shared<Connection::Impl>(session, application)}};
   }
   catch (const system_error& ex)
   {
      co_return std::tuple{ex.code(), Connection{}};
   }
}

} // namespace

IoTask<Connection> connect(Executor executor, std::string host, std::string port, Config config)
{
   co_return co_await connect_to(std::move(executor), std::move(port), std::move(host),
                                 std::move(config));
}

IoTask<Connection> connect(Executor executor, std::vector<TcpEndpoint> endpoints, std::string host,
                           Config config)
{
   co_return co_await connect_to(std::move(executor), std::move(endpoints), std::move(host),
                                 std::move(config));
}

#if ANYHTTP_ASIO
void async_connect_any(Executor executor, std::string host, std::string port, Config config,
                       ConnectHandler&& handler)
{
   auto task = connect(executor, std::move(host), std::move(port), std::move(config));
   launch(executor, std::move(task), std::move(handler));
}
#endif

// =================================================================================================

} // namespace anyhttp::quic
