.. cpp:namespace:: anyhttp

Concurrent requests
===================

HTTP/2 and HTTP/3 multiplex requests: each one is a stream of its own, and streams make progress
independently. HTTP/1.1 has a single connection instead, which requests are written to and responses
read from, one after the other. anyhttp treats HTTP/1.1 as a protocol with **"max concurrent streams
= 1"**, and makes that explicit in the client API instead of hiding it.

Complete requests and responses
-------------------------------

A request is *complete* when all of it, header and body, has been written to the connection:

* A request **without a body** is complete as soon as :cpp:func:`Session::submit` has succeeded.
  Whether a request has a body is a matter of its framing only (`RFC 9112, section 6.3
  <https://www.rfc-editor.org/rfc/rfc9112#section-6.3>`__), never of its method: it has none without
  ``Transfer-Encoding`` and with a ``Content-Length`` of zero or none. As the HTTP/1.1 client sends
  every request without ``Content-Length`` chunked, that means ``Content-Length: 0``.
* **Any other request** is complete when :cpp:func:`Writer::write_eof` has succeeded, even if all of
  a ``Content-Length`` has been written before. The body of a request without one is ended
  implicitly: ``write_eof()`` is an idempotent no-op on it, and writing data fails with
  ``broken_pipe``.

A response is *complete* when it has been read to its end.

Rules for HTTP/1.1
------------------

1. ``submit()`` fails with :cpp:var:`errors::would_block` while the previous request is not complete.
2. :cpp:func:`client::Request::get_response` fails with :cpp:var:`errors::would_block` while the
   responses to earlier requests have not been read completely: otherwise, it would read one of those.
3. When a request can not be completed, as a write fails, or it is released before it is complete,
   the connection is stuck in the middle of a message: every later ``submit()`` fails with
   :cpp:var:`errors::connection_aborted`.
4. When a response can not be read completely, as it is released before its end, or its request is
   released without asking for it, every later ``get_response()`` fails with
   :cpp:var:`errors::connection_aborted`.

Pipelining is still possible: complete requests can be sent before any of their responses have been
read.

Design decisions
----------------

**Fail instead of waiting.** An operation that has to wait for an earlier request or response does
not wait. Very often, the caller waiting is the one who has to finish that earlier request or
response, after the operation returns, and waiting would deadlock. Failing immediately with
``would_block`` turns a hang into an error that can be handled: finish the earlier one, then retry.

**No queueing of submitted requests.** An earlier version queued the header of a request submitted
while the previous one was still incomplete, and sent it as soon as that was complete. That allows
code written for HTTP/2, which submits a couple of requests first and writes their bodies later, to
work unchanged. But it needed a queue of pending requests, writes waiting for a header still on its
way (and not cancellable while doing so), and failures cascading to queued requests. Refusing the
submission keeps it at a single incomplete request per session, which is exactly what the protocol
allows.

**Requests and responses may outlive their session.** The session keeps track of all of its readers
and writers, and detaches them when it goes away. Everything a detached request or response is asked
to do after that completes with an error (``connection_aborted`` for HTTP/1.1 and HTTP/2,
``connection_reset`` for HTTP/3), without touching the connection that is gone.

Stream limits in HTTP/2 and HTTP/3
----------------------------------

HTTP/2 and HTTP/3 have a limit of their own: the peer's ``SETTINGS_MAX_CONCURRENT_STREAMS``, or the
QUIC limit on bidirectional streams. Reaching it is answered the way HTTP/1.1 answers its limit of
one, ``submit()`` failing with ``would_block`` instead of waiting, so one and the same caller works
over all three. A server says what it allows with
:cpp:member:`server::Config::max_concurrent_streams`. Set it to 1, and HTTP/2 and HTTP/3 behave
exactly like HTTP/1.1, which is what ``OneStream.WHEN_limit_is_one_THEN_second_request_reports_would_block``
checks for each of them.

Two things to know about the slot a request occupies:

* **It is held longer than in HTTP/1.1.** A stream counts against the limit until it is closed in
  *both* directions, that is, until its response has been read as well, where HTTP/1.1 is free again
  once the request has been written. So "max concurrent streams = 1" is stricter here than HTTP/1.1
  pipelining: the next request has to wait for the previous *response*, not just for the previous
  request.
* **In HTTP/3 it comes free one round trip late.** The credit for another stream is granted as the
  last one closes, and that ``MAX_STREAMS`` has to reach the client, so a ``submit()`` immediately
  after reading a response may still report ``would_block``. Retrying it is what the contract asks
  for anyway.

Only the number is the peer's to choose. What to do when it is reached is anyhttp's, and the libraries
do not agree: nghttp2 version 2 and ngtcp2 refuse the stream, which is reported as ``would_block``,
while nghttp2 version 1 would accept it and hold it back internally, telling the caller nothing, so
streams in progress are counted here instead.
