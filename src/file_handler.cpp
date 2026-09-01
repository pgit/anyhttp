#include "anyhttp/file_handler.hpp"
#include "anyhttp/formatter.hpp" // IWYU pragma: keep
#include "anyhttp/request_handlers.hpp" // for drain()

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>

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
        m_mtime(other.m_mtime), m_id(other.m_id)
   {
   }
   MappedFile& operator=(MappedFile&& other) noexcept
   {
      std::swap(m_data, other.m_data);
      std::swap(m_size, other.m_size);
      std::swap(m_mtime, other.m_mtime);
      std::swap(m_id, other.m_id);
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
      file.m_id = Identity{st};
      if (file.m_size == 0)
         return file;

      void* data = ::mmap(nullptr, file.m_size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (data == MAP_FAILED)
         return std::unexpected(from_errno(errno));

      ::posix_madvise(data, file.m_size, POSIX_MADV_SEQUENTIAL);
      file.m_data = data;
      return file;
   }

   //
   // What the mapping was made from, so a cached mapping can be checked against the file that is
   // on disk now. st_ino/st_dev catch a replaced file (the usual atomic rename), the rest catches
   // a file modified in place.
   //
   struct Identity
   {
      dev_t dev;
      ino_t ino;
      off_t size;
      decltype(std::declval<struct stat>().st_mtim) mtim;

      explicit Identity(const struct stat& st = {})
         : dev(st.st_dev), ino(st.st_ino), size(st.st_size), mtim(st.st_mtim)
      {
      }
      bool operator==(const Identity& other) const noexcept
      {
         return dev == other.dev && ino == other.ino && size == other.size &&
                mtim.tv_sec == other.mtim.tv_sec && mtim.tv_nsec == other.mtim.tv_nsec;
      }
   };

   size_t size() const noexcept { return m_size; }
   auto mtime() const noexcept { return m_mtime; }
   const Identity& identity() const noexcept { return m_id; }
   std::span<const std::byte> bytes() const noexcept
   {
      return {static_cast<const std::byte*>(m_data), m_size};
   }

private:
   void* m_data = nullptr;
   size_t m_size = 0;
   std::chrono::system_clock::time_point m_mtime;
   Identity m_id{};
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
   co_await response.async_write_eof();
}

//
// Everything serving one request needs, computed once per file instead of once per request: the
// mapping plus the response headers derived from it.
//
struct CachedFile
{
   fs::path resolved;
   MappedFile file;
   std::string last_modified;
   std::string_view content_type;
};

//
// Mapping a file per request costs an mmap()/munmap() pair plus the faults to populate the
// mapping, which measured as the single largest per-request cost when serving a file that is
// already in the page cache -- larger than resolving the path, and larger than the QUIC send
// path itself. Keeping the mapping alive across requests removes all of that.
//
// A hit still stat()s the file, so a file replaced or modified on disk is picked up on the next
// request; that is one syscall instead of the ~17 a full resolve-and-map takes. What a hit does
// *not* redo is resolve(), so re-pointing a symlink along the path is only noticed once the file
// it used to point at changes. That direction is safe -- a stale entry can only keep serving a
// file that already passed the containment check -- but it is why this is a cache of resolved
// paths and not a cache of open files.
//
class FileCache
{
public:
   // Bounded so that a large tree cannot pin unbounded address space; least recently used first.
   static constexpr size_t max_entries = 256;
   static constexpr size_t max_bytes = 64u << 20;

   expected<std::shared_ptr<const CachedFile>> get(const std::string& request_path,
                                                   std::string_view prefix, const fs::path& root)
   {
      if (auto hit = lookup(request_path))
      {
         struct stat st{};
         if (::stat(hit->resolved.c_str(), &st) == 0 &&
             MappedFile::Identity{st} == hit->file.identity())
            return hit;
      }

      //
      // Miss, or the file changed underneath us. Build the entry outside the lock: mapping is the
      // expensive part and two requests racing on the same path may as well both do it.
      //
      const auto resolved = resolve(request_path, prefix, root);
      if (!resolved)
         return std::unexpected(resolved.error());

      auto mapped = MappedFile::open(*resolved);
      if (!mapped)
         return std::unexpected(mapped.error());

      // Read mtime() before the move, rather than relying on argument evaluation order.
      auto last_modified = format_http_date(mapped->mtime());
      auto entry = std::make_shared<const CachedFile>(*resolved, std::move(*mapped),
                                                      std::move(last_modified),
                                                      content_type(*resolved));
      insert(request_path, entry);
      return entry;
   }

private:
   std::shared_ptr<const CachedFile> lookup(const std::string& key)
   {
      const std::lock_guard lock{m_mutex};
      const auto it = m_entries.find(key);
      if (it == m_entries.end())
         return nullptr;
      m_lru.splice(m_lru.begin(), m_lru, it->second.lru); // most recently used first
      return it->second.entry;
   }

   void insert(const std::string& key, std::shared_ptr<const CachedFile> entry)
   {
      const std::lock_guard lock{m_mutex};

      if (const auto it = m_entries.find(key); it != m_entries.end())
      {
         m_bytes -= it->second.entry->file.size();
         m_lru.erase(it->second.lru);
         m_entries.erase(it);
      }

      m_bytes += entry->file.size();
      m_lru.push_front(key);
      m_entries.emplace(key, Slot{std::move(entry), m_lru.begin()});

      //
      // Keep at least the entry just inserted, so a file larger than the byte budget still gets
      // served from the cache rather than being evicted immediately every time.
      //
      while (m_entries.size() > 1 && (m_entries.size() > max_entries || m_bytes > max_bytes))
      {
         const auto victim = m_entries.find(m_lru.back());
         m_bytes -= victim->second.entry->file.size();
         m_entries.erase(victim);
         m_lru.pop_back();
      }
   }

   struct Slot
   {
      std::shared_ptr<const CachedFile> entry;
      std::list<std::string>::iterator lru;
   };

   std::mutex m_mutex;
   std::unordered_map<std::string, Slot> m_entries;
   std::list<std::string> m_lru;
   size_t m_bytes = 0;
};

FileCache g_cache;

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
   co_await drain(request);

   const std::string path = request.url().path();
   const auto entry = g_cache.get(path, prefix, root);
   if (!entry)
   {
      logw("serve_file: {}: {}", path, entry.error().message());
      co_await respond(response, status_for(entry.error()));
      co_return;
   }

   const auto& file = (*entry)->file;
   logd("serve_file: {} ({} bytes)", (*entry)->resolved.native(), file.size());
   co_await response.async_submit(200, fields({{"Content-Length", file.size()},
                                               {"Content-Type", (*entry)->content_type},
                                               {"Last-Modified", (*entry)->last_modified}}));

   //
   // The shared_ptr lives in the coroutine frame, so the mapping stays valid across the write no
   // matter how we leave -- normally, by exception or by cancellation -- even if the entry is
   // evicted or replaced meanwhile. Note that touching a mapped page may block on disk I/O,
   // which no amount of chunking would avoid.
   //
   // Body and the end of it go out in one call: the mapped pages reach the transport by
   // reference, and whatever ends the message -- last chunk, END_STREAM, FIN -- travels with the
   // last of them instead of costing a write of its own. An empty file, which cannot be mmap()ed
   // at all, is then simply an empty buffer and ends the same way.
   //
   co_await response.async_write_eof(asio::buffer(file.bytes()));
}

} // namespace anyhttp
