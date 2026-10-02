#pragma once

#include <boost/asio/io_context.hpp>

#if ANYHTTP_COROSIO
#include <boost/corosio/io_context.hpp>
#endif

// =================================================================================================

//
// Runs \p context until it is out of work, like its run(). In a Debug build, it does so one
// handler at a time with run_one(), printing a separator after each -- in red when one took 10 ms
// or more. That shows which turn of the event loop each log line belongs to.
//
size_t run(boost::asio::io_context& context);
#if ANYHTTP_COROSIO
size_t run(boost::corosio::io_context& context);
#endif

unsigned short get_unused_port(boost::asio::io_context& io);

// =================================================================================================
