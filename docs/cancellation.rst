.. cpp:namespace:: anyhttp

Cancellation
============

There are three ways to stop something that is in progress, and only the first of them is
cancellation:

* **Cancelling** stops one operation, through the mechanism of the I/O runtime. It completes right
  away, whatever that leaves behind.
* **Abandoning** a message or a session, by destroying its handle, ends what it had open, abruptly.
  There is no operation to complete, and nothing goes on in the background.
* **Draining** finishes what was started: writing the rest of a body and ending it, reading one to
  its end, :cpp:func:`Session::shutdown`. These are ordinary operations, which may wait for the peer
  for as long as the peer takes. That is why they are separate, and why a time limit goes around
  them rather than into cancellation.

Cancelling
----------

On ASIO, an operation is cancelled through the cancellation slot of its completion token, which is
what ``cancel_after`` and ``||`` use. On COROSIO, it is cancelled through the ``std::stop_token`` of
the coroutine that awaits it. :cpp:func:`when_either` works on both, see :doc:`programmers-guide`.

There is one kind of cancellation. On ASIO, that is a request with ``cancellation_type::terminal``,
the one ``cancel_after`` sends, and ``||`` sends it as well. A request for ``partial`` or ``total``
alone is ignored, which is what Asio asks of an operation that cannot promise what those stand for.

What a cancelled operation promises
-----------------------------------

1. It **completes right away**. It does not wait for the peer, for flow control, or for a frame to
   be finished, so it never needs a time limit of its own. Its completion is still queued like any
   other, not run from inside the cancellation.
2. It completes with an error that compares equal to ``errc::operation_canceled``, unless it had
   finished already, in which case it reports how it finished.
3. A read reports the bytes it has delivered, a write **how many bytes of its buffer it has taken**.
   Those are no longer the caller's: what is left to write starts after them, at ``buffer + n``.
   Bytes taken need not have reached the peer, as some protocols copy them first.
4. The buffer of the operation has to stay valid until it completes, cancelled or not. Once it has
   completed, nothing refers to it any more.
5. It leaves the message either **usable**, so that it can be drained, or **broken**, so that it can
   only be abandoned. The cancellation does not say which; the next operation does, by failing.

What it leaves behind
---------------------

.. list-table::
   :header-rows: 1
   :stub-columns: 1

   * - Operation
     - After it has been cancelled
   * - :cpp:func:`Client::connect <client::Client::connect>`
     - Nothing: over HTTP/3, the connection being set up ends with it.
   * - :cpp:func:`get_response() <client::Request::get_response>`
     - Usable: it can be started again. Over HTTP/1.1 only if nothing of the response had arrived.
   * - :cpp:func:`Reader::read_some`
     - Usable: what arrives afterwards is read as usual.
   * - :cpp:func:`Writer::write`, :cpp:func:`Writer::write_eof`
     - Depends on the protocol, see below.
   * - :cpp:func:`Session::shutdown`
     - The orderly end is not waited for any more, and the session takes no more requests. Destroy
       it to end the connection.

Whether a cancelled write leaves its body usable depends on how the protocol takes it:

* **HTTP/1.1** sends the body straight out of the caller's buffer, as one chunk, so a write that
  stops in the middle leaves a chunk that can't be finished. The connection stops sending, and every
  further write fails. ``n`` is what of the body has gone onto the connection.
* **HTTP/2** over nghttp2 version 1 copies what flow control lets through. The stream stays usable,
  and ``n`` is what has been copied.
* **HTTP/2** over nghttp2 version 2 and **HTTP/3** copy a request body, a bounded piece at a time,
  so a request stays usable, and ``n`` is what has been copied. A response is handed over by
  reference until the peer has it, see :doc:`http2-library`. If the cancellation leaves bytes of it
  outstanding, the stream is reset and the response is broken; if not, it stays usable, and ``n``
  is what the stack has been given. Plain QUIC streams copy, unless
  :cpp:member:`quic::Config::zero_copy` says to hand over by reference.

Draining
--------

A write that was cancelled with its message left usable is finished by writing ``buffer + n``, the
part it has not taken, and then ending the body, or with ``write_eof(buffer + n)`` in one go. That
holds for a cancelled ``write_eof()`` as well: if the end of the body had not been given to the
protocol stack yet, it is taken back with the write, and if it had been, ending the body again is
the idempotent re-end of :doc:`end-of-body`. Either way, ``write_eof(buffer + n)`` is right.

This needs the ``n`` of the cancelled write. :cpp:func:`when_either` does not hand back the result
of the operation it cancels, so for a write that is to be drained, cancel it through its own token
on ASIO, ``cancel_after`` for a time limit, and with a stop token of its own on COROSIO.

A drain can wait for the peer just like the write before it, so it is where a time limit belongs.
Cancelling the drain is cancellation again: it completes right away, and if the message is broken
then, all that is left is abandoning it.

A coroutine that has been cancelled itself, and wants to drain before it returns, needs
:cpp:func:`reset_cancellation` and :cpp:func:`shielded` to get to its next ``co_await``.

Abandoning
----------

Destroying a :cpp:class:`Writer`, or the request or response it belongs to, before the body has
ended cuts the body short: over HTTP/2 and HTTP/3, the stream is reset. Destroying a
:cpp:class:`Reader` before the end of its body stops reading: over HTTP/3 with a STOP_SENDING, over
HTTP/2 by letting flow control stop the peer, so that a response can still go out, and over
HTTP/1.1, where the rest of a body can't be skipped, by closing the connection.

Destroying a :cpp:class:`Session`, or :cpp:func:`Session::reset`, ends the connection, see
:doc:`programmers-guide`. None of this waits for anything or leaves anything running: what is still
in progress on that message or session completes with an error.
