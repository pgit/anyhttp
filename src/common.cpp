#include <anyhttp/common.hpp>

namespace anyhttp
{

// =================================================================================================

std::string to_string(Protocol protocol)
{
   switch (protocol)
   {
   case Protocol::http11:
      return "HTTP11";
   case Protocol::h2:
      return "HTTP2";
   default:
      return fmt::format("UNKNOWN ({})", std::to_underlying(protocol));
   }
}

std::ostream& operator<<(std::ostream& str, Protocol protocol)
{
   return str << to_string(protocol);
}

// -------------------------------------------------------------------------------------------------

asio::ip::address normalize(asio::ip::address addr)
{
   if (addr.is_v6())
   {
      asio::ip::address_v6 v6 = addr.to_v6();
      if (v6.is_v4_mapped())
         return asio::ip::make_address_v4(asio::ip::v4_mapped, v6);
   }
   return addr;
}

asio::ip::tcp::endpoint normalize(const asio::ip::tcp::endpoint& endpoint)
{
   return {normalize(endpoint.address()), endpoint.port()};
}

}; // namespace anyhttp

// =================================================================================================

std::string what(const std::exception_ptr& ptr)
{
   if (!ptr)
      return "success";
   else
   {
      try
      {
         std::rethrow_exception(ptr);
      }
      catch (boost::system::system_error& ex)
      {
         return fmt::format("exception: {}", ex.code().message());
      }
      catch (std::exception& ex)
      {
         return fmt::format("exception: {}", ex.what());
      }
   }
}

// -------------------------------------------------------------------------------------------------

/// Format according to HTTP date spec (RFC 7231)
std::string format_http_date(std::chrono::system_clock::time_point tp)
{
   using namespace std::chrono;

#if 1
   // cet current system time and convert to UTC
   std::time_t time = system_clock::to_time_t(tp);
   std::tm tm = *std::gmtime(&time);

   constexpr auto months = std::array{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

   return fmt::format("{:02d}, {:02d} {} {:04d} {:02d}:{:02d}:{:02d} GMT", tm.tm_wday, tm.tm_mday,
                      months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
#else
   auto utc_time = floor<seconds>(tp);
   return fmt::format("{:%a, %d %b %Y %H:%M:%S} GMT", utc_time);
#endif
}

// =================================================================================================
