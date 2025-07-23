---
marp: true
author: Peter Eisenlohr
theme: default
class:
  - lead
  - invert
paginate: true
transition: fade
# header: Network Programming with ASIO and C++20 Coroutines
# footer: Network Programming wth ASIO and C++20 Coroutines
---
<style>
   :root {
      --color-background: #101010;
      --color-foreground: #ffffff;
   },
   codenix {
      background-color: black !important;
      color: ghostwhite;
   }
</style>

## Network Programming with ASIO and C++20 Coroutines
Peter Eisenlohr

---
# Overview
* Disclaimer
* Introduction to ASIO
* Synchronous Operations
* Asynchronous Operations
* Completion Tokens
* Coroutines
* Tasks
* Structured Concurrency

---
# Disclaimer
* This presentation is NOT about the C++20 Coroutines Language Feature
* But: To use coroutines with ASIO, you don't need to now all the gory details


---
<!--
   The following is 1:1 output of ChatGPT's answer to:
   "Give a short, one-slide introduction to the ASIO C++ networking library."
   Nicely, it mentions co_await :)
-->
# What is ASIO
* ASIO (Asynchronous Input/Output) is a cross-platform C++ library for network and low-level I/O programming. It provides a consistent asynchronous model using modern C++.
---
* Key Features:
  - Header-only or standalone (no Boost required)
  - Supports synchronous and asynchronous operations
  - Works with sockets, timers, serial ports, and more
  - Integrates naturally with C++ coroutines (co_await)

* Why Use It?
   - Efficient event-driven I/O
   - Scales well for high-performance servers and clients
   - Clean abstraction over platform-specific APIs (epoll, IOCP, etc.)
---


```c++
void session(tcp::socket sock)
{
   std::array<char, 1024> buffer;
   for (;;)
   {
      boost::system::error_code error;
      size_t length = sock.read_some(asio::buffer(data), error);
      if (error == asio::error::eof)
         break;
      asio::write(sock, asio::buffer(data, length));
   }
}

int main(int argc, char* argv[])
{
   asio::io_context io_context;
   tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), port));
   session(acceptor.accept());
   return 0;
}
```
---

```c++
void session(tcp::socket sock)
{
   std::array<char, 1024> buffer;
   for (;;)
   {
      boost::system::error_code error;
      size_t length = sock.read_some(asio::buffer(data), error);
      if (error == asio::error::eof)
         break;
      asio::write(sock, asio::buffer(data, length));
   }
}

int main(int argc, char* argv[])
{
   asio::io_context io_context;
   tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), port));
   for (;;)
      std::thread(session, acceptor.accept()).detach();
   return 0;
}
```

---

# Thoughts
* Using C++ coroutines could be simpler, but there is no library support 
* ASIO is well-established, performant and part of boost
* Create better networking code today

---

# Outlook
## std::execution
* P2300 std::execution
* far away from procedural programming style
* builds graphs of async operations at compile time
* dedicated value, error and cancellation channels
* gives the compiler more opportunities to optimize
* maybe there will be some S/R based networking in the future
* should interop with coroutines

---

* [Collection of Coroutine](https://www.reddit.com/r/cpp/comments/1hqj6ve/feeing_hard_to_understand_coroutine_in_c20_and/)
* [CppCon 2018: G. Nishanov “Nano-coroutines to the Rescue! (Using Coroutines TS, of Course)”](https://www.youtube.com/watch?v=j9tlJAqMV7U)


