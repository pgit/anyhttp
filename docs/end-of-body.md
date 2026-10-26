# The End of a Body

A body ends where the protocol says it ends, and never at a zero-sized transfer. Reading and
writing are not symmetric here: the end of an incoming body is *reported*, the end of an outgoing
one is *stated*.

## Reading

`read_some()` completes with `errors::eof` and zero bytes at the end of the body, and
keeps reporting that for every read after it -- there is no state in which a finished body starts
looking unfinished again. A body cut short completes with `errors::partial_message` instead:
the two are the whole difference between "the peer is done" and "the peer is gone", and neither
degrades into the other.

Reading into a zero-length buffer completes immediately, with success, and says nothing at all
about the body. It is not a way to poll for the end.

## Writing

`write_eof()` ends the body, with or without a buffer of its own (see [Server](../README.md#server)
in the README). `write({})` is a no-op: it succeeds and leaves the body open.

That an empty write means nothing is deliberate. Generic code that forwards whatever it just read
will hand over an empty buffer eventually, and a message that ends by accident is a bug that only
shows up under load. Ending a body is an explicit operation.

## After the end

All three protocols answer in the same order, whatever the state of the connection underneath:

1. An empty non-EOF write succeeds -- always, even after the body has ended.
2. Once the body has ended, writing data fails with `errc::broken_pipe`, whether it comes
   through `write()` or `write_eof(buffer)`. Ending an already ended body without data
   succeeds: it is idempotent.
3. Only then do the errors of a closed or cancelled stream apply. Their codes differ per protocol
   (`connection_aborted` for HTTP/1.1 and HTTP/2, `connection_reset` for HTTP/3).

This order survives the session going away. A detached reader or writer answers from the state it
latched, so a read past a detached stream still distinguishes a clean end (`eof`) from a truncated
one (`partial_message`), without touching the connection that is gone.

## Cancellation

Cancelling `write_eof()` does not take the end of the body back. The end is declared the
moment the operation starts, and no protocol can un-send a FIN; cancellation detaches the handler,
and issuing `write_eof()` again adopts the end that is already on its way rather than
failing. Intent and delivery are tracked separately for exactly this reason.
