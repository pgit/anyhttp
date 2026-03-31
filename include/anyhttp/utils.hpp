#pragma once

#include "anyhttp/common.hpp"

#include <boost/asio/io_context.hpp>

// =================================================================================================

size_t run(boost::asio::io_context& context);

unsigned short get_unused_port(boost::asio::io_context& io);

// =================================================================================================
