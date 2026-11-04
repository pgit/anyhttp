anyhttp
=======

A low-level C++23 HTTP client and server library: HTTP/1.1, HTTP/2 and HTTP/3 behind one
type-erasing interface, hence the *any* in the name, and plain QUIC streams next to HTTP/3 for
protocols of your own. It runs on either `Asio
<https://www.boost.org/doc/libs/release/doc/html/boost_asio.html>`__ or `Corosio
<https://github.com/cppalliance/corosio>`__, chosen when a build is configured.

.. warning::
   **THIS REPOSITORY IS PURELY EXPERIMENTAL.** It is not meant for production use, and anything in it
   can change or break at any time, without notice.

None of the protocols is implemented from scratch. The library is a wrapper around `Beast
<https://github.com/boostorg/beast>`__ (its parser and serializer), `nghttp2
<https://github.com/nghttp2/nghttp2>`__ or `nghttp2 version 2
<https://github.com/nghttp2/nghttp2-v2>`__, and `ngtcp2 <https://github.com/ngtcp2/ngtcp2>`__ with
`nghttp3 <https://github.com/ngtcp2/nghttp3>`__.

:doc:`programmers-guide` is the place to start. It goes through what a program does with the
library, in the order it does it, and links to :doc:`reference` for everything it mentions.

The `source is on GitHub <https://github.com/pgit/anyhttp>`__, and so is `its CI
<https://github.com/pgit/anyhttp/actions/workflows/release.yml>`__, which also makes the `coverage
report <https://pgit.github.io/anyhttp/coverage/>`__.

.. toctree::
   :maxdepth: 2
   :caption: Guide

   programmers-guide

.. toctree::
   :maxdepth: 1
   :caption: How it works

   api-styles
   end-of-body
   concurrent-requests
   alt-svc
   http2-library
   design

.. toctree::
   :maxdepth: 2
   :caption: Reference

   reference

.. toctree::
   :maxdepth: 1
   :caption: Working on anyhttp

   testing
   capture
   devcontainer
   notes
