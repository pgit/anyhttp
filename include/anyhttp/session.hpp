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
   explicit Session(std::shared_ptr<Impl> impl);
   Session(Session&& other) noexcept;
   ~Session();
   Session(const Session& other);

   client::Request submit(boost::urls::url url, Fields headers);

private:
   const std::shared_ptr<Impl> m_impl;
};

// =================================================================================================

} // namespace anyhttp
