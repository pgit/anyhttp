.. cpp:namespace:: anyhttp

The HTTP/2 library
==================

HTTP/2, server and client both, runs on either nghttp2 or its ground-up rewrite, `nghttp2 version 2
<https://github.com/nghttp2/nghttp2-v2>`__, by the same author. The rewrite's API is much closer to
nghttp3's than to version 1's: one ``nghttp2_conn`` instead of a session and an option object,
settings and callbacks as plain structs, callbacks for connection and HTTP events rather than for
frames, bodies taken by reference, and flow control credit extended by hand.

.. list-table::
   :header-rows: 1
   :stub-columns: 1

   * -
     - nghttp2
     - nghttp2 version 2
   * - CMake
     - ``-DH2_LIBRARY=nghttp2`` (default)
     - ``-DH2_LIBRARY=nghttp2v2``
   * - Source
     - ``src/h2/``, ``include/anyhttp/h2/``
     - ``src/h2v2/``, ``include/anyhttp/h2v2/``

This is a choice when a build is configured, not at runtime, because the two cannot share a process:
version 2 installs as its own package but still exports nine of version 1's symbol names, among them
the four ``nghttp2_rcbuf_*`` whose structs differ. A build links one or the other, and compiles only
the backend directory that goes with it.

Nothing above the backend changes: the same :cpp:enumerator:`Protocol::h2`, the same API, and the same
behavior wherever the two libraries leave anyhttp the choice. Where they do not:

* The ``Connection: Upgrade`` from HTTP/1.1 to h2c (`RFC 7540, section 3.2
  <https://www.rfc-editor.org/rfc/rfc7540#section-3.2>`__) went with `RFC 9113
  <https://www.rfc-editor.org/rfc/rfc9113>`__ and has no counterpart in version 2, so such a request is
  declined and served as HTTP/1.1 instead, which is always a valid answer to an upgrade.
* The ``ALTSVC`` frame is gone. An alternative service still arrives as a response header field, which
  is how a server sends one anyway, see :doc:`alt-svc`.

Version 2 has had no release yet, so a build pins a commit of it.

What ``h2v2/`` has that ``h2/`` has no counterpart for is its write mode, because version 2 takes an
outgoing body by reference where version 1 copied it. nghttp2 keeps reading from the vector it was
handed until it has framed all of it, which flow control can postpone indefinitely. So a response is
handed over straight out of the caller's buffer, and a write cancelled with bytes still outstanding
resets the stream, there being no way to un-offer that memory. A request body is copied through a
bounded buffer of the stream's, so that cancelling a write leaves the stream usable and the body can
still be ended afterwards. This is the same split, for the same reason, as HTTP/3 has.
