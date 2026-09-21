#pragma once

//
// Alternative services (RFC 7838): how a server tells a client that the origin it is talking to
// is also reachable somewhere else, over some other protocol. For anyhttp, that is the way from
// HTTP/1.1 or HTTP/2 to HTTP/3 -- QUIC has no in-band upgrade of its own, so an "Alt-Svc" naming
// "h3" is what an HTTP/3 "upgrade" amounts to: the *next* connection is made over QUIC.
//
// This is the parser for the field value alone, shared by the "Alt-Svc" header field and the
// HTTP/2 ALTSVC frame, which carry the very same syntax. What a client does with the result is
// in client_impl.hpp, what a server advertises in server_impl.cpp.
//

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace anyhttp
{

// =================================================================================================

/**
 * One advertised alternative: which protocol to speak, where to reach it, and for how long that
 * is worth remembering.
 */
struct AltService
{
   std::string protocol; ///< ALPN protocol name, percent-decoding undone: "h3", "h2", ...
   std::string host; ///< empty if the alternative is on the host of the origin itself
                     ///< (an IPv6 literal comes without its brackets)
   std::string port; ///< decimal, never empty -- an alt-authority without a port is invalid

   /// The "ma" parameter: how long this may be used without being advertised again.
   std::chrono::seconds max_age{86400}; // RFC 7838, section 3.1: 24 hours unless given

   /// The "persist=1" parameter: the alternative survives a change of network.
   bool persist = false;
};

// -------------------------------------------------------------------------------------------------

/**
 * The parsed value of an "Alt-Svc" header field or an HTTP/2 ALTSVC frame.
 */
struct AltSvc
{
   /// The field value was "clear": everything known about the origin is to be forgotten.
   bool clear = false;

   //
   // The advertised alternatives, in the order they were given, which is the order of the
   // server's preference (RFC 7838, section 3).
   //
   std::vector<AltService> services;

   /// The first alternative offering \p protocol, or nullptr if there is none.
   const AltService* find(std::string_view protocol) const noexcept;
};

/**
 * Parses an "Alt-Svc" field value, which is a list of alternatives, or the single token "clear".
 *
 * Nothing in here is fatal: an alternative that does not parse is skipped, and so is one whose
 * parameters are malformed -- a client is free to ignore what it does not understand (RFC 7838,
 * section 3), and the alternatives it *can* read are still worth having. The result is empty for
 * a field value that yields nothing at all.
 */
AltSvc parse_alt_svc(std::string_view value);

// =================================================================================================

} // namespace anyhttp
