#pragma once

#include "client.hpp"
#include "common.hpp"

#include <boost/url.hpp>

namespace anyhttp
{

// =================================================================================================

class Session
{
public:
   class Impl;
   explicit Session(std::unique_ptr<Impl> impl);
   Session(Session&& other) noexcept;
   ~Session();

   client::Request submit(boost::urls::url url, Headers headers);

private:
   std::unique_ptr<Impl> impl;
};

// =================================================================================================

} // namespace anyhttp
