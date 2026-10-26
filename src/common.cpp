#include <anyhttp/common.hpp>
#include <boost/system/system_error.hpp>

namespace anyhttp
{

// =================================================================================================

std::string to_string(Protocol protocol)
{
   switch (protocol)
   {
   case Protocol::h1:
      return "HTTP11";
   case Protocol::h2:
      return "HTTP2";
   case Protocol::h3:
      return "HTTP3";
   default:
      return std::format("UNKNOWN ({})", std::to_underlying(protocol));
   }
}

std::ostream& operator<<(std::ostream& str, Protocol protocol)
{
   return str << to_string(protocol);
}

// -------------------------------------------------------------------------------------------------

TcpEndpoint normalize(const TcpEndpoint& endpoint)
{
   return {normalize(endpoint.address()), endpoint.port()};
}

static std::string colored(Role role, std::string_view text)
{
   const auto* color = role == Role::server ? "\x1b[1;31m" : "\x1b[1;32m";
   return std::format("{}{}\x1b[0m", color, text);
}

std::string log_prefix(Role role)
{
   return colored(role, role == Role::server ? "server" : "client");
}

std::string log_prefix(Role role, std::string_view protocol, const IpAddress& address,
                       unsigned short port)
{
   const auto addr = normalize(address);
   if (addr.is_v4())
      return std::format("{}:\x1b[35m{}\x1b[0m:{}", colored(role, protocol), addr.to_string(),
                         port);
   else
      return std::format("{}:[\x1b[34m{}\x1b[0m]:{}", colored(role, protocol), addr.to_string(),
                         port);
}

std::string log_prefix(Role role, std::string_view protocol,
                       const std::optional<TcpEndpoint>& remote)
{
   if (!remote)
      return colored(role, protocol);

   return log_prefix(role, protocol, remote->address(), remote->port());
}

}; // namespace anyhttp

// =================================================================================================

std::string what(const anyhttp::error_code& ec) { return ec.message(); }
std::string what(const boost::system::system_error& ex) { return what(ex.code()); }
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
         return std::format("exception: {}", ex.code().message());
      }
      catch (std::exception& ex)
      {
         return std::format("exception: {}", ex.what());
      }
   }
}

// -------------------------------------------------------------------------------------------------

HttpDate format_http_date(std::chrono::system_clock::time_point tp)
{
   using namespace std::chrono;

   constexpr auto first = sys_seconds{sys_days{year{0} / January / 1}};
   constexpr auto last = sys_seconds{sys_days{year{9999} / December / 31}} + days{1} - seconds{1};
   const auto time = std::clamp(floor<seconds>(tp), first, last);

   const auto day = floor<days>(time);
   const auto date = year_month_day{day};
   const auto clock = hh_mm_ss{time - day};

   //
   // By hand: std::format() with seven arguments, gmtime_r() and a heap string took 5k
   // instructions, a sixth of a small HTTP/2 request (docs/benchmark.md).
   //
   constexpr std::string_view weekdays = "SunMonTueWedThuFriSat";
   constexpr std::string_view months = "JanFebMarAprMayJunJulAugSepOctNovDec";

   HttpDate result;
   auto* out = result.chars.data();
   const auto put = [&](std::string_view text) { out = std::ranges::copy(text, out).out; };
   const auto put2 = [&](unsigned value) // two digits
   {
      *out++ = static_cast<char>('0' + value / 10);
      *out++ = static_cast<char>('0' + value % 10);
   };

   const auto y = static_cast<unsigned>(static_cast<int>(date.year()));
   put(weekdays.substr(weekday{day}.c_encoding() * 3, 3));
   put(", ");
   put2(static_cast<unsigned>(date.day()));
   put(" ");
   put(months.substr((static_cast<unsigned>(date.month()) - 1) * 3, 3));
   put(" ");
   put2(y / 100);
   put2(y % 100);
   put(" ");
   put2(clock.hours().count());
   put(":");
   put2(clock.minutes().count());
   put(":");
   put2(clock.seconds().count());
   put(" GMT");
   assert(out == result.chars.data() + result.chars.size());
   return result;
}

HttpDate http_date_now() { return format_http_date(std::chrono::system_clock::now()); }

// =================================================================================================
