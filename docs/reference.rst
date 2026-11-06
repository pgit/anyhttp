API reference
=============

Made from the comments in the public headers. :doc:`programmers-guide` is the place to start, and
says how these go together.

Everything is in the namespace ``anyhttp``, with what is for one side in ``anyhttp::client``,
``anyhttp::server`` and ``anyhttp::quic``. This shows the headers of an ASIO build: on COROSIO, there
are no operations with completion tokens, and the types are the other column of
:ref:`the table <io-runtimes-vocabulary>`.

Client
------

.. code-block:: cpp

   #include <anyhttp/client.hpp>

.. doxygenclass:: anyhttp::client::Client
   :members:

.. doxygenstruct:: anyhttp::client::Config
   :members:

.. doxygenclass:: anyhttp::client::Request
   :members:

.. doxygenclass:: anyhttp::client::Response
   :members:

.. doxygentypedef:: anyhttp::client::Message

Session
-------

.. code-block:: cpp

   #include <anyhttp/session.hpp>

.. doxygenclass:: anyhttp::Session
   :members:

Server
------

.. code-block:: cpp

   #include <anyhttp/server.hpp>

.. doxygenclass:: anyhttp::server::Server
   :members:

.. doxygenstruct:: anyhttp::server::Config
   :members:

.. doxygentypedef:: anyhttp::server::RequestHandler

.. doxygenclass:: anyhttp::server::Request
   :members:

.. doxygenclass:: anyhttp::server::Response
   :members:

Reading and writing
-------------------

.. code-block:: cpp

   #include <anyhttp/reader.hpp>
   #include <anyhttp/writer.hpp>

.. doxygenclass:: anyhttp::Reader
   :members:

.. doxygenclass:: anyhttp::Writer
   :members:

Headers and protocols
---------------------

.. code-block:: cpp

   #include <anyhttp/common.hpp>

.. doxygenenum:: anyhttp::Protocol

.. doxygenfunction:: anyhttp::to_string

.. doxygentypedef:: anyhttp::Fields

.. doxygenfunction:: anyhttp::fields

.. doxygenclass:: anyhttp::FieldValue
   :members:

.. doxygenvariable:: anyhttp::default_max_header_size

.. doxygenfunction:: anyhttp::header_field_size

Plain QUIC streams
------------------

.. code-block:: cpp

   #include <anyhttp/quic.hpp>

.. doxygenfunction:: anyhttp::quic::connect

.. doxygenfunction:: anyhttp::quic::async_connect

.. doxygenstruct:: anyhttp::quic::Config
   :members:

.. doxygenclass:: anyhttp::quic::Connection
   :members:

.. doxygenclass:: anyhttp::quic::Stream
   :members:

.. doxygentypedef:: anyhttp::quic::ConnectionHandler

Alternative services
--------------------

.. code-block:: cpp

   #include <anyhttp/alt_svc.hpp>

.. doxygenfunction:: anyhttp::parse_alt_svc

.. doxygenstruct:: anyhttp::AltSvc
   :members:

.. doxygenstruct:: anyhttp::AltService
   :members:

The runtime
-----------

.. code-block:: cpp

   #include <anyhttp/runtime.hpp>

What the interface is written in. The names are the same on both I/O runtimes, and what each
stands for is the runtime's own, given here for both, as in :ref:`the table <io-runtimes-vocabulary>`.

Types
^^^^^

.. cpp:type:: template <typename T = void> anyhttp::Task

   A coroutine, as the runtime runs it.

   :ASIO: ``boost::asio::awaitable<T>``
   :COROSIO: ``boost::capy::task<T>``

.. cpp:type:: template <typename... Ts> anyhttp::IoTask

   An operation, as the coroutine that yields its :cpp:type:`results`: ``IoTask<size_t>`` is
   ``Task<results<size_t>>``, ``IoTask<>`` one that reports only its error.

   :ASIO: ``boost::asio::awaitable<std::tuple<error_code, Ts...>>``
   :COROSIO: ``boost::capy::task<boost::capy::results<Ts...>>``

.. cpp:type:: template <typename... Ts> anyhttp::results

   What an operation yields: its error first, then what it has to report besides, which
   ``auto [ec, n] = co_await reader.read_some(buffer);`` takes apart. Whether that is meaningful
   when there is an error is up to the operation.

   :ASIO: ``std::tuple<error_code, Ts...>``
   :COROSIO: ``boost::capy::results<Ts...>``, which is ``std::tuple<std::error_code, Ts...>``

.. cpp:type:: anyhttp::error_code

   What every operation reports its outcome with.

   :ASIO: ``boost::system::error_code``
   :COROSIO: ``std::error_code``

.. cpp:type:: anyhttp::system_error

   What the runtime throws an :cpp:type:`error_code` as.

   :ASIO: ``boost::system::system_error``
   :COROSIO: ``std::system_error``

.. cpp:type:: anyhttp::Executor

   What sessions, streams and their readers and writers run on.

   :ASIO: ``boost::asio::any_io_executor``
   :COROSIO: ``boost::capy::any_executor``

.. cpp:type:: anyhttp::IpAddress

   The runtime's own IP address, which the API speaks, too.

   :ASIO: ``boost::asio::ip::address``
   :COROSIO: ``boost::corosio::ip_address``

.. cpp:type:: anyhttp::TcpEndpoint

   The runtime's own TCP endpoint.

   :ASIO: ``boost::asio::ip::tcp::endpoint``
   :COROSIO: ``boost::corosio::endpoint``

.. cpp:type:: anyhttp::UdpEndpoint

   The runtime's own UDP endpoint.

   :ASIO: ``boost::asio::ip::udp::endpoint``
   :COROSIO: ``boost::corosio::endpoint``, the same type as :cpp:type:`TcpEndpoint`

.. doxygentypedef:: anyhttp::DefaultCompletionToken

.. doxygenfunction:: anyhttp::throw_error

Errors
^^^^^^

The codes anyhttp reports where the two runtimes spell them differently. Anything else compares
against ``errc::``, which is ``boost::system::errc`` on ASIO and ``std::errc`` on COROSIO.

.. doxygenvariable:: anyhttp::errors::eof

.. doxygenvariable:: anyhttp::errors::partial_message

.. doxygenvariable:: anyhttp::errors::canceled

.. doxygenvariable:: anyhttp::errors::would_block

.. doxygenvariable:: anyhttp::errors::connection_aborted

.. doxygenvariable:: anyhttp::errors::bad_descriptor

.. doxygenvariable:: anyhttp::errors::already_started

.. doxygenvariable:: anyhttp::errors::header_limit

Coroutines
^^^^^^^^^^

.. doxygenfunction:: anyhttp::launch(const Executor &executor, Task<void> task)

.. doxygenfunction:: anyhttp::launch(const Executor &executor, Task<void> task, F &&on_done)

.. doxygenfunction:: anyhttp::delay

.. doxygenfunction:: anyhttp::when_both

.. doxygenfunction:: anyhttp::when_either

.. doxygenfunction:: anyhttp::reset_cancellation

.. doxygenfunction:: anyhttp::shielded
