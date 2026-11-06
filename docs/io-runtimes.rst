.. cpp:namespace:: anyhttp

I/O runtimes: Asio and Corosio
==============================

A build of the library is on one of two I/O runtimes, chosen when it is configured:

* **ASIO**, on `Asio <https://www.boost.org/doc/libs/release/doc/html/boost_asio.html>`__: every
  operation is an asynchronous operation of Asio, and takes any completion token. It is also there
  as a coroutine.
* **COROSIO**, on `Corosio <https://github.com/cppalliance/corosio>`__: coroutines only.

Each brings the sockets, timers and TLS, on top of an async runtime: the executors, coroutines and
cancellation. Asio is both in one. Corosio is the I/O only, on top of `Capy
<https://github.com/cppalliance/capy>`__, which is the async runtime.

Both build the same library and protocol sources. Only a thin runtime layer differs,
``anyhttp/runtime.hpp`` and ``anyhttp/net.hpp`` with what they include from ``asio/`` or ``corosio/``.
:src:`docs/corosio-port-plan.md` has how that came about.

.. _io-runtimes-vocabulary:

What the names stand for
------------------------

.. list-table::
   :header-rows: 1
   :stub-columns: 1

   * -
     - ASIO
     - COROSIO
   * - CMake
     - ``-DANYHTTP_API=ASIO`` (default)
     - ``-DANYHTTP_API=COROSIO -DTLS_LIBRARY=OpenSSL``
   * - :cpp:type:`Task\<T\> <Task>`
     - ``asio::awaitable<T>``
     - ``capy::task<T>``
   * - :cpp:type:`Executor`
     - ``asio::any_io_executor``
     - ``capy::any_executor``
   * - :cpp:type:`error_code`
     - ``boost::system::error_code``
     - ``std::error_code``
   * - :cpp:type:`results\<Ts...\> <results>`
     - ``std::tuple<error_code, Ts...>``
     - ``capy::results<Ts...>``
   * - :cpp:type:`IoTask\<Ts...\> <IoTask>`
     - ``asio::awaitable<results<Ts...>>``
     - ``capy::task<results<Ts...>>``
   * - :cpp:type:`TcpEndpoint`, :cpp:type:`UdpEndpoint`
     - ``asio::ip::tcp::endpoint``, ``asio::ip::udp::endpoint``
     - ``corosio::endpoint``
   * - Operations
     - ``async_x(..., token)`` and ``x()``
     - ``x()``
   * - TLS
     - AWS-LC (default) or OpenSSL 3.5+
     - OpenSSL 3.5+
   * - Programs
     - ``server``, ``client``
     - ``server``

Buffers are Asio's on both, headers are Beast's :cpp:type:`Fields`, URLs Boost.URL's.

The reference shows what the headers have for ASIO, as Doxygen is told to read them that way. So the
reference has the operations with completion tokens that a COROSIO build does not have, and the types
as they are in the table. ``corosio/runtime.hpp`` has the same names as ``asio/runtime.hpp``, in the
same order.

.. _io-runtimes-operations:

The shape of an operation
-------------------------

On ASIO, an operation comes in two shapes. The asynchronous operation of Asio is named ``async_...``
and takes a completion token last, which decides what it returns, and the coroutine of the plain name
wraps it. On COROSIO, the coroutine is all there is. Taking :cpp:func:`Reader::read_some` for an
example, with the types of the library:

.. code-block:: cpp

   // ASIO: the operation, which completes a handler of the signature void(error_code, size_t) ...
   template <typename CompletionToken = DefaultCompletionToken>
   auto async_read_some(asio::mutable_buffer buffer, CompletionToken&& token = CompletionToken());

   // ... and the coroutine around it.
   IoTask<size_t> read_some(asio::mutable_buffer buffer);

   // COROSIO: the coroutine.
   IoTask<size_t> read_some(asio::mutable_buffer buffer);

The coroutine has the same declaration on both, so code that uses only coroutines builds with
either, which is what the guide does. It yields :cpp:type:`results`, a tuple with the error first,
and reports what goes wrong instead of throwing it; :cpp:type:`IoTask\<size_t\> <IoTask>` is short
for ``Task<results<size_t>>``. Every operation has it: ``read_some``, ``write``, ``write_eof``,
``submit``, ``get_response``, ``connect``, ``get``, and the others. What it does not have is a
completion token, so an ASIO build that wants one, for a callback, for ``cancel_after`` or to have
errors thrown, uses the ``async_...`` shape, see :ref:`completion-tokens` in the guide.

With the types spelled out, the same piece of code on ASIO, using both shapes:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [shape-asio]
   :end-before: // [shape-asio-end]

And on COROSIO:

.. literalinclude:: snippets.cpp
   :language: cpp
   :start-after: // [shape-corosio]
   :end-before: // [shape-corosio-end]

On ASIO, the implementation of each operation is hidden behind `any_completion_handler
<https://www.boost.org/doc/libs/release/doc/html/boost_asio/reference/any_completion_handler.html>`__,
so that it can be compiled separately. On COROSIO, they are coroutines of `Capy
<https://github.com/cppalliance/capy>`__, Corosio's base, cancelled through the awaiting chain's
``std::stop_token``. :cpp:class:`Reader` and :cpp:class:`Writer` model ``capy::ReadStream`` and
``capy::WriteStream``.

.. _io-runtimes-errors:

What the errors are
-------------------

Where the two runtimes spell an error differently, the namespace ``anyhttp::errors`` names it:

.. list-table::
   :header-rows: 1
   :stub-columns: 1

   * -
     - ASIO
     - COROSIO
   * - :cpp:var:`errors::eof`
     - ``asio::error::eof``
     - ``capy::error::eof``
   * - :cpp:var:`errors::partial_message`
     - ``beast::http::error::partial_message``
     - ``capy::error::stream_truncated``
   * - :cpp:var:`errors::canceled`
     - ``errc::operation_canceled``
     - ``capy::error::canceled``
   * - :cpp:var:`errors::would_block`
     - ``asio::error::would_block``
     - ``errc::operation_would_block``
   * - :cpp:var:`errors::connection_aborted`
     - ``asio::error::connection_aborted``
     - ``errc::connection_aborted``
   * - :cpp:var:`errors::header_limit`
     - ``beast::http::error::header_limit``
     - the same

Anything else compares against ``errc::`` (``boost::system::errc`` or ``std::errc``), with one
exception: a host that does not resolve is ``netdb_errors::host_not_found`` on ASIO and
``errc::no_such_device_or_address`` on COROSIO.
