#include "anyhttp/file_handler.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/request_handlers.hpp" // for send()

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <span>

using namespace std::string_view_literals;
using namespace anyhttp;
using boost::system::error_code;

// =================================================================================================

namespace
{

namespace fs = std::filesystem;

error_code from_errno(int error) { return {error, boost::system::system_category()}; }

//
// A file mapped into memory for as long as this object lives. An empty file maps to an empty
// span: mmap() rejects a zero length, and there is nothing to send anyway.
//
class MappedFile
{
public:
   MappedFile() = default;
   MappedFile(MappedFile&& other) noexcept
      : m_data(std::exchange(other.m_data, nullptr)), m_size(std::exchange(other.m_size, 0)),
        m_mtime(other.m_mtime)
   {
   }
   MappedFile& operator=(MappedFile&& other) noexcept
   {
      std::swap(m_data, other.m_data);
      std::swap(m_size, other.m_size);
      std::swap(m_mtime, other.m_mtime);
      return *this;
   }
   ~MappedFile()
   {
      if (m_data)
         ::munmap(m_data, m_size);
   }

   static expected<MappedFile> open(const fs::path& path)
   {
      const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0)
         return std::unexpected(from_errno(errno));
      auto close = defer([fd] { ::close(fd); });

      struct stat st{};
      if (::fstat(fd, &st) < 0)
         return std::unexpected(from_errno(errno));

      //
      // A directory can be open()ed, but not mapped -- and we do not serve listings anyway.
      // Anything else that is not a regular file (FIFO, device, socket) has no size to speak of.
      //
      if (!S_ISREG(st.st_mode))
         return std::unexpected(from_errno(S_ISDIR(st.st_mode) ? EISDIR : EINVAL));

      MappedFile file;
      file.m_size = static_cast<size_t>(st.st_size);
      file.m_mtime = std::chrono::system_clock::from_time_t(st.st_mtime);
      if (file.m_size == 0)
         return file;

      void* data = ::mmap(nullptr, file.m_size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (data == MAP_FAILED)
         return std::unexpected(from_errno(errno));

      ::posix_madvise(data, file.m_size, POSIX_MADV_SEQUENTIAL);
      file.m_data = data;
      return file;
   }

   size_t size() const noexcept { return m_size; }
   auto mtime() const noexcept { return m_mtime; }
   std::span<const std::byte> bytes() const noexcept
   {
      return {static_cast<const std::byte*>(m_data), m_size};
   }

private:
   void* m_data = nullptr;
   size_t m_size = 0;
   std::chrono::system_clock::time_point m_mtime;
};

//
// Map the part of the request path below 'prefix' onto 'root'. weakly_canonical() resolves ".."
// and symlinks, so comparing the result against the canonical root catches any attempt to reach
// outside of it.
//
expected<fs::path> resolve(std::string_view path, std::string_view prefix, const fs::path& root)
{
   const auto reject = std::unexpected(from_errno(ENOENT));

   if (!path.starts_with(prefix))
      return reject;
   path.remove_prefix(prefix.size());
   if (!path.empty() && !path.starts_with('/'))
      return reject; // 'prefix' matched in the middle of a segment, e.g. "/testament" for "/test"

   while (path.starts_with('/'))
      path.remove_prefix(1);

   std::error_code ec;
   const auto base = fs::weakly_canonical(root, ec);
   if (ec)
      return std::unexpected(from_errno(ec.value()));

   const auto file = fs::weakly_canonical(base / fs::path(path), ec);
   if (ec)
      return std::unexpected(from_errno(ec.value()));

   const auto relative = file.lexically_relative(base);
   if (relative.empty() || *relative.begin() == "..")
      return reject;

   return file;
}

std::string_view content_type(const fs::path& path)
{
   static constexpr std::pair<std::string_view, std::string_view> types[] = {
      {".css", "text/css"},        {".gif", "image/gif"},         {".htm", "text/html"},
      {".html", "text/html"},      {".jpeg", "image/jpeg"},       {".jpg", "image/jpeg"},
      {".js", "text/javascript"},  {".json", "application/json"}, {".pdf", "application/pdf"},
      {".png", "image/png"},       {".svg", "image/svg+xml"},     {".txt", "text/plain"},
      {".xml", "application/xml"}, {".zip", "application/zip"}};

   auto extension = path.extension().string();
   std::ranges::transform(extension, extension.begin(),
                          [](unsigned char ch) { return std::tolower(ch); });

   const auto it =
      std::ranges::find(types, extension, &std::pair<std::string_view, std::string_view>::first);
   return it == std::ranges::end(types) ? "application/octet-stream"sv : it->second;
}

unsigned status_for(const error_code& ec)
{
   switch (ec.value())
   {
   case ENOENT:
   case ENOTDIR:
   case EISDIR:
   case ENAMETOOLONG:
      return 404;
   case EACCES:
   case EPERM:
      return 403;
   default:
      return 500;
   }
}

awaitable<void> respond(server::Response& response, unsigned status)
{
   co_await response.async_submit(status, fields({{"Content-Length", 0}}));
   co_await response.async_write({});
}

} // namespace

namespace anyhttp
{

awaitable<void> serve_file(server::Request request, server::Response response, fs::path root,
                           std::string prefix)
{
   //
   // Read the request body to EOF before responding. A GET normally carries none, but HTTP/1.1
   // has to close the connection when a handler leaves the request unparsed, which would
   // truncate the response we are about to write.
   //
   std::array<uint8_t, 4096> discard;
   while (co_await request.async_read_some(asio::buffer(discard)) > 0)
      ;

   const std::string path = request.url().path();
   const auto resolved = resolve(path, prefix, root);
   if (!resolved)
   {
      logw("serve_file: {}: {}", path, resolved.error().message());
      co_await respond(response, status_for(resolved.error()));
      co_return;
   }

   const auto file = MappedFile::open(*resolved);
   if (!file)
   {
      logw("serve_file: {}: {}", resolved->native(), file.error().message());
      co_await respond(response, status_for(file.error()));
      co_return;
   }

   logd("serve_file: {} ({} bytes)", resolved->native(), file->size());
   co_await response.async_submit(200,
                                  fields({{"Content-Length", file->size()},
                                          {"Content-Type", content_type(*resolved)},
                                          {"Last-Modified", format_http_date(file->mtime())}}));

   //
   // The mapping lives in the coroutine frame, so it stays valid across the write and is
   // unmapped no matter how we leave -- normally, by exception or by cancellation. Note that
   // touching a mapped page may block on disk I/O, which no amount of chunking would avoid.
   //
   if (file->size() > 0)
      co_await send(response, file->bytes()); // an empty write already means EOF, don't send two

   co_await response.async_write({});
}

} // namespace anyhttp
