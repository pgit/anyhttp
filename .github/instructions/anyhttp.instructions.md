---
applyTo: '**'
---
AnyHTTP is a type-erased interface for implementing HTTP Servers and Clients. It offers common
interfaces for HTTP/1.1, HTTP/2 and HTTP/3 (QUIC, not implemented yet).

AnyHTTP is completely asynchronous and intended to be used with C++20 coroutines.
It uses Boost ASIO as the underlying async runtime and tries to adhere to it's
[Async Model](https://think-async.com/Asio/boost_asio_1_30_2/doc/html/boost_asio/overview/model.html).
