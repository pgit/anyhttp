#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <string>
#include <system_error>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

capy::task<>
server_once(corosio::io_context& ioc, corosio::tcp_acceptor& acc, std::string& out)
{
   corosio::tcp_socket peer(ioc);

   auto [accept_ec] = co_await acc.accept(peer);
   if (accept_ec)
      throw std::system_error(accept_ec, "accept");

   std::array<char, 1024> buffer{};
   auto [read_ec, n] = co_await peer.read_some(capy::mutable_buffer(buffer.data(), buffer.size()));
   if (read_ec)
      throw std::system_error(read_ec, "read_some");

   out.assign(buffer.data(), n);
}

capy::task<>
client_once(corosio::io_context& ioc, corosio::endpoint endpoint, std::string payload)
{
   corosio::tcp_socket sock(ioc);

   auto [connect_ec] = co_await sock.connect(endpoint);
   if (connect_ec)
      throw std::system_error(connect_ec, "connect");

   std::size_t total = 0;
   while (total < payload.size())
   {
      auto [write_ec, n] = co_await sock.write_some(
         capy::const_buffer(payload.data() + total, payload.size() - total));
      if (write_ec)
         throw std::system_error(write_ec, "write_some");
      total += n;
   }
}

int main()
{
   corosio::io_context ioc;

   corosio::tcp_acceptor acceptor(ioc, corosio::endpoint(corosio::ipv4_address::loopback(), 0));
   corosio::endpoint endpoint = acceptor.local_endpoint();

   std::string received;
   std::string payload = "hello over corosio + capy";

   std::exception_ptr first_error;
   int completed = 0;

   auto done = [&]() {
      ++completed;
      if (completed == 2)
         ioc.stop();
   };

   auto failed = [&](std::exception_ptr ep) {
      if (!first_error)
         first_error = ep;
      done();
   };

   auto ex = ioc.get_executor();

   capy::run_async(ex, done, failed)(server_once(ioc, acceptor, received));
   capy::run_async(ex, done, failed)(client_once(ioc, endpoint, payload));

   ioc.run();

   if (first_error)
      std::rethrow_exception(first_error);

   if (received != payload)
   {
      std::cerr << "payload mismatch: expected '" << payload << "', got '" << received << "'\n";
      return 2;
   }

   std::cout << "ok: transferred '" << received << "'\n";
   return 0;
}
