#pragma once

#include "anyhttp/server.hpp"

#include <filesystem>
#include <string>

namespace anyhttp
{

// =================================================================================================

/**
 * Serve a single file from below \p root, mapped into memory with mmap(). The part of the request
 * path below \p prefix is taken as the path relative to \p root; anything that would escape
 * \p root ("..", a symlink pointing outside) is rejected with 404.
 */
awaitable<void> serve_file(server::Request request, server::Response response,
                           std::filesystem::path root, std::string prefix);

// =================================================================================================

} // namespace anyhttp
