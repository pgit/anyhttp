# Overview
Low-level C++ HTTP client and server library, based on ASIO.

It supports HTTP/1.x, HTTP/2 and HTTP/3 behind a common, type-erasing interface, hence the *any* int the name.

None of those protocols are implemented from scratch. Instead, it is a wrapper around the following established libraries:

* Boost Beast
* nghttp2
* nghttp3



## Links

For now, this section contains just a set of random links collected during delevopment.

* [Beast Example using Type Erasure](https://www.boost.org/doc/libs/develop/boost/beast/http/message_generator.hpp)

