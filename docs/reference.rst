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

What the interface is written in, and what each name is on ASIO. On COROSIO the same names are
something else, which :ref:`the table <io-runtimes-vocabulary>` has.

.. doxygentypedef:: anyhttp::Task

.. doxygentypedef:: anyhttp::Executor

.. doxygentypedef:: anyhttp::IpAddress

.. doxygentypedef:: anyhttp::TcpEndpoint

.. doxygentypedef:: anyhttp::UdpEndpoint

.. doxygentypedef:: anyhttp::error_code

.. doxygentypedef:: anyhttp::system_error

.. doxygentypedef:: anyhttp::DefaultCompletionToken

.. doxygenfunction:: anyhttp::throw_error

Errors
^^^^^^

Where the two runtimes spell an error differently, see :ref:`the table <io-runtimes-errors>`. Anything
else compares against ``errc::``.

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
