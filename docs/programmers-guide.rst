.. cpp:namespace:: anyhttp

Programmer's guide
==================

This goes through what a program does with the library, from its first request to shutting down.
The code is taken from :src:`docs/examples` and :src:`docs/snippets.cpp`, which are compiled with
every build, and :src:`test/test_docs.cpp` runs it over all three protocols. What is about how it
all works rather than how it is used has a page of its own, starting with :doc:`api-styles`.

.. contents:: On this page
   :local:
   :depth: 2

Getting the library
-------------------

It takes C++23, CMake 3.30 and Boost 1.88 or later, spdlog, nghttp2, ngtcp2 and nghttp3. The
Dockerfiles in :src:`.devcontainer` build all of them, and those are the versions that are tested,
see :doc:`devcontainer`. Whatever else goes into the process has to agree with anyhttp on the
standard library and the TLS library.

As a subproject, which builds the library and nothing else, in both API styles:

.. code-block:: cmake

   include(FetchContent)
   FetchContent_Declare(anyhttp
      GIT_REPOSITORY https://github.com/pgit/anyhttp.git
      GIT_TAG <commit>
      SYSTEM)
   set(TLS_LIBRARY OpenSSL) # the options of the API style and the libraries, if the defaults won't do
   FetchContent_MakeAvailable(anyhttp)

   target_link_libraries(my_program PRIVATE anyhttp::anyhttp)

``ANYHTTP_BUILD_TESTS``, ``ANYHTTP_BUILD_APPS``, ``ANYHTTP_BUILD_DOCS`` and ``ANYHTTP_INSTALL``
default to ``OFF`` that way. To develop against a checkout, set ``FETCHCONTENT_SOURCE_DIR_ANYHTTP``
to it.

Or installed, which is for the ASIO style only: Corosio writes install rules only when it is the
top-level project, so a library that links it can't be exported.

.. code-block:: sh

   cmake --preset build-deploy
   cmake --build --preset build-deploy
   cmake --install build-deploy --prefix /opt/anyhttp

.. code-block:: cmake

   find_package(anyhttp 0.0 REQUIRED) # with /opt/anyhttp in CMAKE_PREFIX_PATH
   target_link_libraries(my_program PRIVATE anyhttp::anyhttp)

``anyhttp_API``, ``anyhttp_TLS_LIBRARY`` and ``anyhttp_H2_LIBRARY`` then say what that copy was
built with. The package has the public headers only, those of the protocol backends stay behind.
It looks for its dependencies where they were when it was built.

A build is of one API style or the other, ``-DANYHTTP_API=ASIO``, the default, or ``COROSIO``, which
also takes ``-DTLS_LIBRARY=OpenSSL``. :doc:`api-styles` has the difference. Everything below uses
what both have and builds with either, and what is for ASIO only says so.

A first server
--------------

The whole of a program, which answers every request:

.. literalinclude:: examples/hello_server.cpp
   :language: cpp
   :start-at: #include <anyhttp/server.hpp>

A :cpp:class:`server::Server` is made on an executor, and from then on everything it does happens
there. There is no thread behind it, so nothing happens unless the context runs. It starts
listening as it is made, on TCP and UDP, on the same port, and serves HTTP/1.1, HTTP/2 and HTTP/3
from there, with TLS and without. So there is nothing to choose: try it with ``curl``, which takes
the test PKI that the build makes in ``pki/out``.

.. code-block:: sh

   build/docs/hello_server &
   curl http://localhost:8080/
   curl --cacert pki/out/root.pem https://localhost:8080/
   curl --cacert pki/out/root.pem --http3-only https://localhost:8080/

The handler gets a :cpp:class:`server::Request` and a :cpp:class:`server::Response`, by value: each
one a move-only handle, whose end is what the peer gets to see. Each request is a coroutine of its
own, and on HTTP/2 and HTTP/3 any number of them run at the same time.

Every server loads a certificate, also one that is meant to serve cleartext only. The defaults of
:cpp:member:`tls_certificate_chain <server::Config::tls_certificate_chain>` and
:cpp:member:`tls_private_key <server::Config::tls_private_key>` are the test PKI, relative to the
working directory, which is good for development and for nothing else. Without either, making the
server throws.

A first client
--------------

The whole of a program, which makes one GET and prints what comes back:

.. literalinclude:: examples/hello_client.cpp
   :language: cpp
   :start-at: #include <anyhttp/client.hpp>

A :cpp:class:`client::Client` makes connections, and :cpp:func:`connect() <client::Client::connect>`
gives each as a :cpp:class:`Session`, which is what requests are submitted on. There is no error to
catch: like every operation that both API styles have, these yield a tuple with the error first, and
report what goes wrong instead of throwing it. The library has something to say about it all, too,
which goes to the default logger of spdlog, see `Logging`_.

.. code-block:: sh

   build/docs/hello_client http://localhost:8080/
   build/docs/hello_client https://localhost:8080/ pki/out/root.pem

Serving requests
----------------

Reading and writing a body
^^^^^^^^^^^^^^^^^^^^^^^^^^

This echoes what it gets, as it gets it:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [echo]
   :end-before: // [echo-end]

:cpp:func:`Writer::content_length` says how long the body will be, before
:cpp:func:`submit() <server::Response::submit>` sends the header. It can be said with a header field
as well, and what is said neither way goes out as a chunked body over HTTP/1.1.

The end of an incoming body is reported the way the runtime reports it everywhere else:
:cpp:var:`errors::eof` with zero bytes. A body cut short by a reset stream, or a connection that
went away in the middle of a message, completes with :cpp:var:`errors::partial_message` instead, so
the two stay distinguishable.

The end of an *outgoing* body is stated explicitly, with :cpp:func:`Writer::write_eof`. It takes a
buffer of its own, so the last of the body and the end of it go out together: one DATA frame with
END_STREAM, one QUIC STREAM frame with FIN, one last chunk, instead of costing a second, empty write.

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [write-at-once]
   :end-before: // [write-at-once-end]

:cpp:func:`fields` is how to write a few header fields in place, and takes numbers as well as
strings. :doc:`end-of-body` has what every operation answers after the end.

Telling requests apart
^^^^^^^^^^^^^^^^^^^^^^

A server has one handler, and what it does with the method, the URL and the fields of a request is
up to it. :cpp:func:`get_param_as() <server::Request::get_param_as>` looks up a parameter of the
query and converts it:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [route]
   :end-before: // [route-end]

Handing the request and the response on, as this does, is all it takes, as they are nothing but
handles. So a handler may also give the reading half to one coroutine and the writing half to
another, and :cpp:class:`server::Request` converts to a :cpp:class:`Reader` for the purpose, and
:cpp:class:`server::Response` to a :cpp:class:`Writer`.

Limits
^^^^^^

:cpp:member:`max_header_size <server::Config::max_header_size>` bounds the header section of a
request, which is answered with 431 if it has more, without the handler seeing it.
:cpp:member:`max_concurrent_streams <server::Config::max_concurrent_streams>` bounds the requests
that one connection has in progress over HTTP/2 and HTTP/3, see :doc:`concurrent-requests`. Bodies
are bounded by flow control: what nobody reads, nobody sends more of.

Making requests
---------------

One request, one operation
^^^^^^^^^^^^^^^^^^^^^^^^^^

:cpp:func:`Session::get` does a whole GET as one operation, and hands back the response as a plain
Beast message, with status, fields and the body as a ``std::string``:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [get]
   :end-before: // [get-end]

A response that says 404 is not an error but a response, and arrives as one. The convenience is paid
for with memory, as the body is buffered in full. Anything that wants to look at the body while it
arrives, or to send a body of its own, goes through :cpp:func:`Session::submit`.

A request in four steps
^^^^^^^^^^^^^^^^^^^^^^^

:cpp:func:`Session::submit` sends the header and yields a :cpp:class:`client::Request`, whose body
:cpp:func:`Writer::write_eof` ends, and :cpp:func:`client::Request::get_response` waits for the
header of the response. That gives a :cpp:class:`client::Response`, which is a
:cpp:class:`Reader` for the rest:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [post]
   :end-before: // [post-end]

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [read-all]
   :end-before: // [read-all-end]

``submit()`` is a POST unless it is given a method. A request without a ``Content-Length`` has a
chunked body over HTTP/1.1, except for GET and HEAD, which go out with ``Content-Length: 0`` unless
their fields frame a body.

Stopping early
^^^^^^^^^^^^^^

Reading a response from a server that may send more than you want stops wherever you do, with status
and header fields in hand:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [partial]
   :end-before: // [partial-end]

What the server gets to send in the meantime is bounded by flow control, and over HTTP/1.1 by the
socket buffers. Releasing the response early also resets its stream with HTTP/2 and HTTP/3, leaving
the session usable, but it is the end of an HTTP/1.1 connection.

Writing and reading at the same time
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Writing a whole body and only then reading the response is fine for a body that is small, or a
server that reads it to its end first. For one that answers while it reads, as the echo above does,
the two would wait for each other as soon as all the buffers on the way are full, so the two have to
happen at the same time, which :cpp:func:`when_both` is for:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [both-ways]
   :end-before: // [both-ways-end]

The helpers in here report what goes wrong by throwing, as that is what makes ``when_both()`` cancel
the other one.

Several requests on one session
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

HTTP/2 and HTTP/3 multiplex requests, up to a limit that the peer sets, and HTTP/1.1 has a limit of
one, with some pipelining. In all of them, ``submit()`` at the limit does not wait: the caller may be
the one who has to finish the request that is in the way. It fails with
:cpp:var:`errors::would_block`, and a caller who knows that something else will finish that request
can retry later:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [would-block]
   :end-before: // [would-block-end]

:doc:`concurrent-requests` has the rules, and over TLS, :cpp:func:`Session::protocol` says what the
server has picked. There is also a limit on the header section of a response,
:cpp:member:`client::Config::max_header_size`.

What a client connects to
-------------------------

An ``https`` URL means TLS, with the server's certificate verified against the default trust store
of the TLS library:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [tls]
   :end-before: // [tls-end]
   :dedent:

or against :cpp:member:`tls_ca_file <client::Config::tls_ca_file>`, as a PEM file, which is read on
every connect:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [private-ca]
   :end-before: // [private-ca-end]
   :dedent:

:cpp:member:`protocol <client::Config::protocol>` then says what to offer with ALPN. The default
``h2`` also offers HTTP/1.1, and the server picks. Any other scheme means cleartext, and exactly the
protocol given, which for HTTP/2 means prior knowledge. HTTP/3 is TLS over QUIC with any scheme:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [cleartext]
   :end-before: // [cleartext-end]
   :dedent:

The URL says what the connection is for and, unless told otherwise, also where it goes. A caller with
a resolver of its own, or following a DNS HTTPS record, gives the addresses in
:cpp:member:`endpoints <client::Config::endpoints>`, which are tried in order. Nothing is resolved
then, while the certificate still has to be valid for the host of the URL, and requests still say
they are for it:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [endpoints]
   :end-before: // [endpoints-end]
   :dedent:

To get from HTTP/1.1 or HTTP/2 to HTTP/3, see :doc:`alt-svc`. Over TLS, a body that ends with the
connection is complete only if the server has said so with a ``close_notify``: otherwise, it may
have been cut by anyone, and reading it fails with :cpp:var:`errors::partial_message`.

Errors
------

What goes wrong is an :cpp:type:`error_code`. Where the two runtimes spell one differently, the
namespace ``errors::`` names it, see :ref:`the table <api-styles-errors>`, and anything else compares
against ``errc::``: ``ec == errc::connection_reset``. The ones that belong to the contract
of an operation are in its reference and in :doc:`end-of-body` and :doc:`concurrent-requests`.

Requests and responses may outlive their session. It keeps track of them, and detaches them when it
goes away, and from then on everything they are asked to do completes with an error, without touching
the connection that is gone.

What a handler throws ends the coroutine of that request. On HTTP/1.1, which has nothing smaller to
abort, it also ends the connection, so a handler that means to go on has to catch it.

Cancellation
------------

How an operation is cancelled depends on the API style: with a cancellation slot, from a completion
token, or through the ``std::stop_token`` of the awaiting coroutine. What works in both is racing it
against something else, with :cpp:func:`when_either`, which cancels the loser:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [timeout]
   :end-before: // [timeout-end]

A cancelled operation completes with an error that compares equal to ``errc::operation_canceled``,
which is the way to ask, as a timer of the runtime has its own spelling of it. What that leaves behind
depends on how far it had come, and on the protocol:

* A cancelled :cpp:func:`get_response() <client::Request::get_response>` can be started again.
* Cancelling :cpp:func:`Writer::write_eof` does not take the end of the body back, see
  :ref:`end-of-body-cancellation`.
* A write that has started putting data on a TCP connection leaves part of a message behind. Over
  HTTP/1.1, that is the end of the connection for further requests, see
  :doc:`concurrent-requests`.
* The response of a server over HTTP/2 version 2 and HTTP/3 is handed to the protocol stack by
  reference, without a copy, until the peer has it. As that can't be taken back, cancelling a write
  with bytes outstanding resets the stream. A request body is copied, so it can go on afterwards.

A coroutine that has been cancelled and wants to clean up by awaiting something else, as a last
``write_eof()``, needs :cpp:func:`reset_cancellation` and :cpp:func:`shielded` to get there.

Completion tokens
-----------------

**ASIO only.** Every operation is also an asynchronous operation of Asio, named ``async_...``, which
takes any completion token. There is no need for one in a coroutine. Without one, what goes wrong is
thrown:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [token-exception]
   :end-before: // [token-exception-end]

With ``as_tuple`` it is reported, the way the coroutine spelling does:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [token-tuple]
   :end-before: // [token-tuple-end]

A callback is a token as well:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [token-callback]
   :end-before: // [token-callback-end]

So is ``cancel_after``, which is the short way to a timeout on this API style:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [cancel-after]
   :end-before: // [cancel-after-end]

Ending a session
----------------

There are two ways to end a session, and neither waits for what is in progress, which fails right
away:

* :cpp:func:`Session::shutdown` ends the connection in an orderly way: over HTTP/2 with a GOAWAY, over
  TLS with a ``close_notify`` in both directions, then a FIN in both, with each wait limited to two
  seconds. It fails with :cpp:var:`errors::connection_aborted` only if a write had to be interrupted,
  which rules out a ``close_notify``. HTTP/3 sends its CONNECTION_CLOSE.
* Destroying the session, or :cpp:func:`Session::reset`, is over as fast as possible, and does nothing
  asynchronous. The peer sees a connection that was cut.

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [shutdown]
   :end-before: // [shutdown-end]

Running it
----------

Stopping
^^^^^^^^

Destroying the :cpp:class:`server::Server` stops it listening and ends every connection it has. Do
that on the server's own executor, and then let the context run on until it returns:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [stop]
   :end-before: // [stop-end]

A context has to run dry, in both API styles. What is suspended when it is destroyed mostly gets
unwound, but not what waits on an object that only comes apart by running, and sessions do. HTTP/2
and HTTP/3 leak without that drain.

Threads
^^^^^^^

Everything is on one thread unless told otherwise, which needs no locks and no strands. With
several, there are two ways:

* one context, run by all threads, and :cpp:member:`use_strand <server::Config::use_strand>` to give
  each connection a strand of its own, on which its handlers run:

  .. literalinclude:: snippets.cpp
     :language: cpp
     :start-after: // [threads]
     :end-before: // [threads-end]

* a context on each thread, each with a server of its own, and
  :cpp:member:`reuse_port <server::Config::reuse_port>` to have them all on the same port. On Asio,
  this is what scales best, see :src:`docs/benchmark.md`. The kernel spreads connections and QUIC datagrams
  over them, by address, so all of them have to be bound before traffic arrives, and a QUIC client
  whose address changes lands on another server, which does not know it.

Either way, a :cpp:class:`Session` and everything it gives out are to be used on its executor only.
The ``server`` program does both, see ``--threads`` and ``--independent`` in
:src:`src/apps/server_main.cpp`.

On Corosio, one context on several threads has a use-after-free in its reactor, whose window is
narrow but real, see step 8 of :src:`docs/corosio-port-plan.md`.

Logging
^^^^^^^

The library and the programs log through spdlog's default logger, so its level and its sinks are what
to set. The logger is also where to look first: each line carries the protocol, the peer and the
stream it belongs to, which is the same tag as :cpp:func:`Reader::log_prefix` gives.

Plain QUIC streams
------------------

QUIC streams without HTTP in them, for a protocol of your own told apart by its ALPN, run on the UDP
port HTTP/3 is on. A :cpp:class:`quic::Stream` is a :cpp:class:`Reader` and a :cpp:class:`Writer` at
once, and :cpp:func:`server::Server::on_quic` is what gets each connection:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [quic-server]
   :end-before: // [quic-server-end]
   :dedent:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [quic-echo]
   :end-before: // [quic-echo-end]

and :cpp:func:`quic::connect` is the other side of that:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [quic-client]
   :end-before: // [quic-client-end]

The ends map onto the contract of :doc:`end-of-body`:

.. list-table::
   :header-rows: 1

   * - QUIC
     - Writer's side
     - Reader's side
   * - FIN
     - :cpp:func:`write_eof() <Writer::write_eof>`
     - :cpp:var:`errors::eof`
   * - RESET_STREAM
     - :cpp:func:`reset_stream() <quic::Stream::reset_stream>`, or dropping the Writer before the end
     - :cpp:var:`errors::partial_message`
   * - STOP_SENDING
     - writes fail with ``errc::connection_reset``
     - :cpp:func:`stop_sending() <quic::Stream::stop_sending>`, or dropping the Reader before the end

The two directions are independent. Opening a stream never waits: at the peer's limit,
:cpp:func:`open_stream() <quic::Connection::open_stream>` fails with :cpp:var:`errors::would_block`.
A connection is closed when its handle goes away, on the server when the handler returns.
:cpp:struct:`quic::Config` has the flow control limits and how writes reach the stack, and
:src:`docs/quic-streams-plan.md` has how all this came about.

Deploying the programs
----------------------

``cmake --install build-deploy --prefix <dir> --component Runtime`` gives ``bin/anyhttp-server`` and
``bin/anyhttp-client``, plus every shared library they need in ``lib/``, except for glibc and
``libgcc_s``. The tree can be moved or copied to another machine that has a glibc at least as new as
the build host's. The bundled libraries have licenses of their own, to be passed on along with them.

.. code-block:: sh

   anyhttp-server -p 443 --cert /etc/anyhttp/chain.pem --key /etc/anyhttp/key.pem

The server always loads a certificate, also to serve only cleartext: without ``--cert`` and ``--key``
it looks for the test PKI (``pki/out/``, relative to the working directory), and exits with an error
if that isn't there. :src:`scripts/check_package.sh` verifies all of this from the outside.
