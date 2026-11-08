.. cpp:namespace:: anyhttp

The end of a body
=================

A body ends where the protocol says it ends, and never at a zero-sized transfer. Reading and writing
are not symmetric here: the end of an incoming body is *reported*, the end of an outgoing one is
*stated*.

Reading
-------

:cpp:func:`Reader::read_some` completes with :cpp:var:`errors::eof` and zero bytes at the end of the
body, and keeps reporting that for every read after it: there is no state in which a finished body
starts looking unfinished again. A body cut short completes with
:cpp:var:`errors::partial_message` instead. The two are the whole difference between "the peer is
done" and "the peer is gone", and neither degrades into the other.

Reading into a zero-length buffer completes immediately, with success, and says nothing at all about
the body. It is not a way to poll for the end.

Writing
-------

:cpp:func:`Writer::write_eof` ends the body, with or without a buffer of its own, see
:doc:`programmers-guide`. :cpp:func:`write({}) <Writer::write>` is a no-op: it succeeds and leaves the
body open.

That an empty write means nothing is deliberate. Generic code that forwards whatever it just read
will hand over an empty buffer eventually, and a message that ends by accident is a bug that only
shows up under load. Ending a body is an explicit operation.

After the end
-------------

All three protocols answer in the same order, whatever the state of the connection underneath:

1. An empty non-EOF write succeeds, always, even after the body has ended.
2. Once the body has ended, writing data fails with ``errc::broken_pipe``, whether it comes through
   ``write()`` or ``write_eof(buffer)``. Ending an already ended body without data succeeds: it is
   idempotent.
3. Only then do the errors of a closed or cancelled stream apply. Their codes differ per protocol:
   ``connection_aborted`` for HTTP/1.1 and HTTP/2, ``connection_reset`` for HTTP/3.

This order survives the session going away. A detached reader or writer answers from the state it
latched, so a read past a detached stream still distinguishes a clean end (``eof``) from a truncated
one (``partial_message``), without touching the connection that is gone.

.. _end-of-body-cancellation:

Cancellation
------------

Cancelling ``write_eof()`` takes the end of the body back if the protocol stack has not been given
it yet: the body is open again, and what the write has not taken can follow. Once the stack has it,
the end stands, as no protocol can un-send a FIN, and issuing ``write_eof()`` again is the
idempotent re-end of rule 2 -- or, over HTTP/3, adopts the end that is still on its way. Which of the
two a cancelled ``write_eof()`` leaves depends on how far it had come, so going on with
``write_eof(buffer + n)``, where ``n`` is what it has taken, is right either way. See
:doc:`cancellation`.
