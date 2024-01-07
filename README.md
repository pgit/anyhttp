# Overview
Low-level C++ HTTP client and server library, based on ASIO.

It supports HTTP/1.x, HTTP/2 and HTTP/3 behind a common, type-erasing interface, hence the *any* int the name.

None of those protocols are implemented from scratch. Instead, it is a wrapper around the following established libraries:

* Boost Beast
* nghttp2
* nghttp3

