#pragma once

#include "anyhttp/common.hpp" // IWYU pragma: keep

#include "anyhttp/client_impl.hpp"
#include "anyhttp/server_impl.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>

#include <utility>
#include <vector>

namespace anyhttp::beast_impl
{

// =================================================================================================

template <typename Stream>
class BeastSession : public ::anyhttp::Session::Impl
{
protected:
   BeastSession(std::string_view log_prefix, Executor executor, Stream&& stream);

public:
   ~BeastSession() override;

   std::string_view log_prefix() const { return log_prefix_; }

   // ----------------------------------------------------------------------------------------------

   void destroy() noexcept override;

   Executor get_executor() const noexcept override { return executor_; }

   // ----------------------------------------------------------------------------------------------

   //
   // Readers and writers refer back to their session, and may outlive it. So each of them
   // registers here for as long as it exists, to be detach()ed when the session goes away first.
   // With pipelining, a client session may have more than one of each at a time.
   //
   void attach(Reader::Impl& reader) { readers_.push_back(&reader); }
   void attach(Writer::Impl& writer) { writers_.push_back(&writer); }
   void release(Reader::Impl& reader) { std::erase(readers_, &reader); }
   void release(Writer::Impl& writer) { std::erase(writers_, &writer); }

   void detach_readers()
   {
      for (auto* reader : std::exchange(readers_, {}))
         reader->detach();
   }

   void detach_writers()
   {
      for (auto* writer : std::exchange(writers_, {}))
         writer->detach();
   }

   /// Called once by each reader when it is done with the stream: either because its message has
   /// been read completely (\p complete), or because it is going away before that.
   virtual void reader_finished(bool complete) {}

   // ----------------------------------------------------------------------------------------------

public:
   std::string log_prefix_;
   Executor executor_;
   Stream stream_;
   Buffer buffer_;
   bool closed_ = false;

private:
   /// Non-owning pointers to the attached readers, see attach().
   std::vector<Reader::Impl*> readers_;

   /// Non-owning pointers to the attached writers, see attach().
   std::vector<Writer::Impl*> writers_;
};

// =================================================================================================

class ServerSessionBase
{
public:
   inline ServerSessionBase(server::Server::Impl& parent) : server_(&parent) {}
   server::Server::Impl& server()
   {
      assert(server_);
      return *server_;
   }

private:
   server::Server::Impl* server_ = nullptr;
};

template <typename Stream>
class ServerSession : public ServerSessionBase, public BeastSession<Stream>
{
   using super = BeastSession<Stream>;

   // FIXME: maybe use CRTP or something similar to avoid this?
   using super::buffer_;
   using super::closed_;
   using super::detach_readers;
   using super::detach_writers;
   using super::log_prefix;
   using super::stream_;

public:
   ServerSession(server::Server::Impl& parent, Executor executor, Stream&& stream);

   void destroy() noexcept override;
   void async_submit(SubmitHandler&& handler, std::string_view method, boost::urls::url url,
                     const Fields& headers) override;
   Task<void> do_session(Buffer&& data) override;

private:
   /// Takes over the stream after an upgrade to h2c, see do_session().
   std::shared_ptr<Session::Impl> upgraded_;
};

// -------------------------------------------------------------------------------------------------

class ClientSessionBase
{
public:
   inline ClientSessionBase(client::Client::Impl& parent) : client_(&parent) {}
   client::Client::Impl& client()
   {
      assert(client_);
      return *client_;
   }

private:
   client::Client::Impl* client_ = nullptr;
};

template <typename Stream>
class RequestWriter;

/**
 * HTTP/1.1 client session.
 *
 * HTTP/1.1 is treated as a protocol with "max concurrent streams = 1", in the sense that there is
 * only a single connection to write requests to and read responses from, without any means to
 * interleave them. Pipelining is supported: requests can be written before the responses to the
 * earlier ones have been read. But everything that goes onto the connection, or comes off it, has
 * to be done one after the other, and completely. See README.md, "Concurrent Requests", for the
 * reasoning behind this.
 *
 * A request is \e complete when all of it has been written to the connection, header and body:
 *
 * - A request that has no body is complete when async_submit() has succeeded. Whether a request
 *   has a body is a matter of its framing only (RFC 9112, section 6.3), not of its method: it has
 *   none without 'Transfer-Encoding' and with a 'Content-Length' of zero or none. As a request
 *   without 'Content-Length' is always sent chunked, that means "Content-Length: 0".
 * - Any other request is complete when async_write_eof() has succeeded -- even if all of a
 *   'Content-Length' has been written before.
 *
 * A response is \e complete when it has been read to its end.
 *
 * Operations that would have to wait for an earlier request or response don't. Waiting would
 * deadlock as soon as the earlier one is taken care of by the same code, after the operation that
 * waits for it. Instead, they fail immediately with \c asio::error::would_block, so they can be
 * retried later:
 *
 * - async_submit(), while the previous request is not complete yet, and
 * - async_get_response(), while the responses to earlier requests have not been read completely.
 *
 * When a request can not be completed -- a write fails, or the request goes away without being
 * complete -- nothing can be sent after it any more: later calls to async_submit() fail with
 * \c asio::error::connection_aborted. Likewise, when a response can not be read completely -- a
 * read fails, the response goes away before it has been read, or a request goes away without
 * asking for its response -- getting any later response fails.
 */
template <typename Stream>
class ClientSession : public ClientSessionBase, public BeastSession<Stream>
{
   using super = BeastSession<Stream>;

   // FIXME: maybe use CRTP or something similar to avoid this?
   using super::buffer_;
   using super::log_prefix;
   using super::stream_;

public:
   ClientSession(client::Client::Impl& parent, Executor executor, Stream&& stream);

   void async_submit(SubmitHandler&& handler, std::string_view method, boost::urls::url url,
                     const Fields& headers) override;
   Task<std::tuple<error_code, client::Request>> submit(std::string method, boost::urls::url url,
                                                        Fields headers) override;
   Task<void> do_session(Buffer&& data) override;

   // ----------------------------------------------------------------------------------------------

   /// Called by the request that is being sent when it is complete.
   void request_complete(RequestWriter<Stream>& request);

   /// Called by the request that is being sent when it can't be completed any more.
   void request_failed(RequestWriter<Stream>& request);

   /// Called by every request that goes away while the session is still there.
   void request_released(RequestWriter<Stream>& request);

   void reader_finished(bool complete) override;

   // ----------------------------------------------------------------------------------------------

   /// The request that is not complete yet, if any. There can be only one.
   RequestWriter<Stream>* sending_ = nullptr;

   size_t requests_sent_ = 0; ///< number of requests submitted
   size_t responses_read_ = 0; ///< number of responses that have been read completely
   bool send_failed_ = false; ///< set when a request could not be completed
   bool receive_failed_ = false; ///< set when a response could not be read completely
};

// =================================================================================================

} // namespace anyhttp::beast_impl