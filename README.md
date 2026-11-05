
# Overview
[![Build and run tests](https://github.com/pgit/anyhttp/actions/workflows/release.yml/badge.svg)](https://github.com/pgit/anyhttp/actions/workflows/release.yml)
[![Documentation](https://img.shields.io/badge/documentation-read-blue)](https://pgit.github.io/anyhttp/)
[![Coverage](https://img.shields.io/badge/coverage-report-blue)](https://pgit.github.io/anyhttp/coverage/)

Low-level C++23 HTTP client and server library, in one of two API styles, chosen at configure time:

* **ASIO**, on [Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html): every
  operation is an asynchronous operation of Asio and takes any completion token.
* **COROSIO**, on [Corosio](https://github.com/cppalliance/corosio): coroutines only.

> [!WARNING]
> This repository is purely experimental. It is not meant for production use, and anything in it
> can change or break at any time, without notice.

It supports HTTP/1.x, HTTP/2 and HTTP/3 behind a common, type-erasing interface, hence the *any* in the name -- and plain QUIC streams next to HTTP/3, for protocols of your own.

None of those protocols are implemented from scratch. Instead, it is a wrapper around the following well-established libraries:

* [beast](https://github.com/boostorg/beast) (its parser and serializer)
* [nghttp2](https://github.com/nghttp2/nghttp2), or [nghttp2 version 2](https://github.com/nghttp2/nghttp2-v2), its rewrite
* [ngtcp2](https://github.com/ngtcp2/ngtcp2) / [nghttp3](https://github.com/ngtcp2/nghttp3)

This project started as a hand-written replacement for [nghttp2-asio](https://github.com/nghttp2/nghttp2-asio). Once the concept was working, HTTP/1.1 was added, and both protocols were hidden behind a type-erased interface. Finally, HTTP/3 was added with the help of agentic coding.

## Documentation

**<https://pgit.github.io/anyhttp/>** has the [programmer's guide](docs/programmers-guide.rst), which
is the place to start, the API reference and how it all works:
[API styles](docs/api-styles.rst), [the end of a body](docs/end-of-body.rst),
[concurrent requests](docs/concurrent-requests.rst), [moving to HTTP/3](docs/alt-svc.rst) and
[the HTTP/2 library](docs/http2-library.rst). [docs/README.md](docs/README.md) says how to make it.

## Example

A server, which echoes every request, over HTTP/1.1, HTTP/2 and HTTP/3 on one port:

```C++
Task<void> echo(server::Request request, server::Response response)
{
   if (request.content_length())
      response.content_length(request.content_length().value());

   if (auto [ec] = co_await response.submit(200, {}); ec)
      co_return;

   std::array<uint8_t, 64 * 1024> buffer;
   for (;;)
   {
      auto [ec, n] = co_await request.read_some(asio::buffer(buffer));
      if (ec == errors::eof)
         break;
      if (ec)
         co_return;

      if (auto [write_ec] = co_await response.write(asio::buffer(buffer, n)); write_ec)
         co_return;
   }

   co_await response.write_eof();
}
```

A client, for which a plain GET is one operation, and anything else a few more:

```C++
Task<void> do_session(client::Client& client, boost::urls::url url)
{
   auto [ec1, session] = co_await client.connect();

   auto [ec2, message] = co_await session.get(url);
   std::println("{}: {} bytes", message.result_int(), message.body().size());

   auto [ec3, request] = co_await session.submit(url, {});
   auto [ec4] = co_await request.write_eof();
   auto [ec5, response] = co_await request.get_response();
}
```

Both API styles have every operation as such a coroutine, that yields a tuple and reports errors
instead of throwing. On ASIO, each is also an asynchronous operation that takes any completion token.
The whole programs are [hello_server.cpp](docs/examples/hello_server.cpp) and
[hello_client.cpp](docs/examples/hello_client.cpp).

## Using it from a project

C++23, CMake 3.30 and Boost 1.88 or later, spdlog, nghttp2, ngtcp2 and nghttp3 -- the Dockerfiles in
[.devcontainer](.devcontainer) build all of them, and those are the versions that are tested.

```cmake
include(FetchContent)
FetchContent_Declare(anyhttp
   GIT_REPOSITORY https://github.com/pgit/anyhttp.git
   GIT_TAG <commit>
   SYSTEM)
FetchContent_MakeAvailable(anyhttp)

target_link_libraries(my_program PRIVATE anyhttp::anyhttp)
```

An installed package works as well, on ASIO, see
[getting the library](docs/programmers-guide.rst#getting-the-library).

## Links

For now, this section contains just a set of random links collected during development.

* [Beast Example using Type Erasure](https://www.boost.org/doc/libs/develop/boost/beast/http/message_generator.hpp)
* [Development container](docs/devcontainer.rst) -- how the build environment is put together
* [Capturing traffic](docs/capture.rst) -- tcpdump and tshark, decrypting TLS and QUIC with a key log
* [cpp-devcontainer](https://github.com/pgit/cpp-devcontainer) -- the generic C++ base image it builds on
* [c-ares-async](https://github.com/pgit/c-ares-async) -- asynchronous DNS, to resolve HTTPS resource records and start off with HTTP/3
