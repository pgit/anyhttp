.. cpp:namespace:: anyhttp

API styles
==========

A build of the library is of one of two API styles, chosen when it is configured:

* **ASIO**, on `Asio <https://www.boost.org/doc/libs/release/doc/html/boost_asio.html>`__: every
  operation is an asynchronous operation of Asio, and takes any completion token. It is also there
  as a coroutine.
* **COROSIO**, on `Corosio <https://github.com/cppalliance/corosio>`__: coroutines only.

Both styles build the same library and protocol sources. Only a thin runtime layer differs,
``anyhttp/runtime.hpp`` and ``anyhttp/net.hpp`` with what they include from ``asio/`` or ``corosio/``.
:src:`docs/corosio-port-plan.md` has how that came about.

.. _api-styles-vocabulary:

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

Both styles have every operation as a coroutine, ``read_some``, ``write``, ``write_eof``, ``submit``,
``get_response``, ``connect``, ``get``, that yields a tuple and reports errors instead of throwing.
Code written like that builds with either, which is what the guide does. Buffers are Asio's in both,
headers are Beast's :cpp:type:`Fields`, URLs Boost.URL's.

The reference shows what the headers have for ASIO, as Doxygen is told to read them that way. So the
reference has the operations with completion tokens that a COROSIO build does not have, and the types
as they are in the table. ``corosio/runtime.hpp`` has the same names as ``asio/runtime.hpp``, in the
same order.

On ASIO, the implementation of each operation is hidden behind `any_completion_handler
<https://www.boost.org/doc/libs/release/doc/html/boost_asio/reference/any_completion_handler.html>`__,
so that it can be compiled separately. On COROSIO, they are coroutines of `Capy
<https://github.com/cppalliance/capy>`__, Corosio's base, cancelled through the awaiting chain's
``std::stop_token``. :cpp:class:`Reader` and :cpp:class:`Writer` model ``capy::ReadStream`` and
``capy::WriteStream``.

.. _api-styles-errors:

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
