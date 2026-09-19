#include "anyhttp/alt_svc.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <utility>

namespace anyhttp
{

// =================================================================================================

namespace
{

/// A token character, as defined for HTTP field values (RFC 9110, section 5.6.2).
constexpr bool is_tchar(char c) noexcept
{
   constexpr std::string_view special = "!#$%&'*+-.^_`|~";
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          special.find(c) != std::string_view::npos;
}

/// Optional whitespace, which may appear around the separators of a field value.
constexpr bool is_ows(char c) noexcept { return c == ' ' || c == '\t'; }

// -------------------------------------------------------------------------------------------------

/**
 * Undoes the percent-encoding an ALPN protocol name is advertised with (RFC 7838, section 3):
 * anything outside the token characters has to be escaped, so "h3" may arrive as "%68%33".
 *
 * A "%" that is not followed by two hex digits is not an escape and stays as it is.
 */
std::string pct_decode(std::string_view in)
{
   std::string out;
   out.reserve(in.size());
   for (size_t i = 0; i < in.size(); ++i)
   {
      unsigned value = 0;
      const auto* first = in.data() + i + 1;
      const auto* last = first + 2;
      if (in[i] == '%' && i + 2 < in.size() &&
          std::from_chars(first, last, value, 16) == std::from_chars_result{last, std::errc{}})
      {
         out.push_back(static_cast<char>(value));
         i += 2;
      }
      else
         out.push_back(in[i]);
   }
   return out;
}

/**
 * Splits an alt-authority, "[uri-host] ':' port", into host and port. The host may be empty,
 * meaning the host of the origin itself, or an IPv6 literal in brackets, which brings colons of
 * its own -- so the port is looked for behind the closing bracket, and the brackets are taken
 * off: what is handed out is a host a resolver can be given as it is.
 */
bool split_authority(std::string_view authority, std::string& host, std::string& port)
{
   size_t colon = std::string_view::npos;
   bool literal = false;
   if (authority.starts_with('['))
   {
      if (auto bracket = authority.find(']'); bracket != std::string_view::npos)
      {
         colon = authority.find(':', bracket);
         literal = true;
      }
   }
   else
      colon = authority.rfind(':');

   if (colon == std::string_view::npos)
      return false;

   const auto digits = authority.substr(colon + 1);
   if (digits.empty() || !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; }))
      return false;

   auto uri_host = authority.substr(0, colon);
   if (literal)
      uri_host = uri_host.substr(1, uri_host.size() - 2);

   host = uri_host;
   port = digits;
   return true;
}

// -------------------------------------------------------------------------------------------------

/// Just enough of a field value parser for the grammar of RFC 7838, section 3.
class Parser
{
public:
   explicit Parser(std::string_view input) noexcept : m_input(input) {}

   bool eof() const noexcept { return m_pos >= m_input.size(); }
   bool next_is(char c) const noexcept { return !eof() && m_input[m_pos] == c; }

   void skip_ows() noexcept
   {
      while (!eof() && is_ows(m_input[m_pos]))
         ++m_pos;
   }

   bool consume(char c) noexcept
   {
      if (!next_is(c))
         return false;
      ++m_pos;
      return true;
   }

   std::string_view token() noexcept
   {
      const auto start = m_pos;
      while (!eof() && is_tchar(m_input[m_pos]))
         ++m_pos;
      return m_input.substr(start, m_pos - start);
   }

   /// Reads a quoted string and returns its content, with the backslash escapes resolved.
   std::optional<std::string> quoted_string()
   {
      if (!consume('"'))
         return std::nullopt;

      std::string result;
      while (!eof())
      {
         char c = m_input[m_pos++];
         if (c == '"')
            return result;
         if (c == '\\' && !eof())
            c = m_input[m_pos++];
         result.push_back(c);
      }

      return std::nullopt; // unterminated
   }

   /// A token or a quoted string, which is what a parameter value may be.
   std::optional<std::string> token_or_quoted_string()
   {
      if (next_is('"'))
         return quoted_string();
      if (auto value = token(); !value.empty())
         return std::string(value);
      return std::nullopt;
   }

   //
   // Moves past the next comma that is not inside a quoted string, so that parsing can go on with
   // the next alternative after one that could not be read.
   //
   void skip_element() noexcept
   {
      bool quoted = false;
      while (!eof())
      {
         const char c = m_input[m_pos++];
         if (!quoted)
         {
            if (c == ',')
               return;
            quoted = c == '"';
         }
         else if (c == '\\' && !eof())
            ++m_pos;
         else if (c == '"')
            quoted = false;
      }
   }

private:
   std::string_view m_input;
   size_t m_pos = 0;
};

// -------------------------------------------------------------------------------------------------

/// Parses one alt-value: an alternative and the parameters that go with it.
std::optional<AltService> parse_alt_value(Parser& parser)
{
   const auto protocol = parser.token();
   if (protocol.empty() || !parser.consume('='))
      return std::nullopt;

   const auto authority = parser.quoted_string();
   if (!authority)
      return std::nullopt;

   AltService service;
   service.protocol = pct_decode(protocol);
   if (!split_authority(*authority, service.host, service.port))
      return std::nullopt;

   //
   // Parameters. A repeated one is ignored (RFC 7838, section 3: they "MUST NOT occur more than
   // once"), as is one we don't know.
   //
   bool have_max_age = false;
   bool have_persist = false;
   for (parser.skip_ows(); parser.consume(';'); parser.skip_ows())
   {
      parser.skip_ows();
      const auto name = parser.token();
      if (name.empty() || !parser.consume('='))
         return std::nullopt;

      const auto value = parser.token_or_quoted_string();
      if (!value)
         return std::nullopt;

      if (name == "ma" && !std::exchange(have_max_age, true))
      {
         const auto* last = value->data() + value->size();
         if (uint64_t seconds = 0; std::from_chars(value->data(), last, seconds) ==
                                   std::from_chars_result{last, std::errc{}})
            service.max_age = std::chrono::seconds{seconds};
      }
      else if (name == "persist" && !std::exchange(have_persist, true))
         service.persist = *value == "1";
   }

   return service;
}

} // namespace

// =================================================================================================

const AltService* AltSvc::find(std::string_view protocol) const noexcept
{
   for (const auto& service : services)
      if (service.protocol == protocol)
         return &service;

   return nullptr;
}

// -------------------------------------------------------------------------------------------------

AltSvc parse_alt_svc(std::string_view value)
{
   //
   // "clear" is the whole field value and nothing else -- as a list element, it is just an
   // alternative with a missing alt-authority, and ignored like any other malformed one.
   //
   auto trimmed = value;
   while (!trimmed.empty() && is_ows(trimmed.front()))
      trimmed.remove_prefix(1);
   while (!trimmed.empty() && is_ows(trimmed.back()))
      trimmed.remove_suffix(1);
   if (trimmed == "clear")
      return {.clear = true};

   AltSvc result;
   Parser parser(value);
   for (;;)
   {
      parser.skip_ows();
      if (parser.eof())
         break;

      //
      // The list may have empty elements, which are skipped (RFC 9110, section 5.6.1).
      //
      if (parser.consume(','))
         continue;

      if (auto service = parse_alt_value(parser))
      {
         result.services.push_back(std::move(*service));

         //
         // An alternative has to be followed by a comma or the end of the field value. Anything
         // else means we are out of step with the sender, and the rest of this element goes.
         //
         parser.skip_ows();
         if (parser.eof() || parser.consume(','))
            continue;
      }

      parser.skip_element();
   }

   return result;
}

// =================================================================================================

} // namespace anyhttp
