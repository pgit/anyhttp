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
      : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
        mtime_(other.mtime_), id_(other.id_)
   {
   }
   MappedFile& operator=(MappedFile&& other) noexcept
   {
      std::swap(data_, other.data_);
      std::swap(size_, other.size_);
      std::swap(mtime_, other.mtime_);
      std::swap(id_, other.id_);
      return *this;
   }
   ~MappedFile()
   {
      if (data_)
         ::munmap(data_, size_);
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
      file.size_ = static_cast<size_t>(st.st_size);
      file.mtime_ = std::chrono::system_clock::from_time_t(st.st_mtime);
      file.id_ = Identity{st};
      if (file.size_ == 0)
         return file;

      void* data = ::mmap(nullptr, file.size_, PROT_READ, MAP_PRIVATE, fd, 0);
      if (data == MAP_FAILED)
         return std::unexpected(from_errno(errno));

      ::posix_madvise(data, file.size_, POSIX_MADV_SEQUENTIAL);
      file.data_ = data;
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
         return std::tie(this->dev, this->ino, this->size, this->mtim.tv_sec, this->mtim.tv_nsec) ==
                std::tie(other.dev, other.ino, other.size, other.mtim.tv_sec, other.mtim.tv_nsec);
      }
   };

   size_t size() const noexcept { return size_; }
   auto mtime() const noexcept { return mtime_; }
   const Identity& identity() const noexcept { return id_; }
   std::span<const std::byte> bytes() const noexcept
   {
      return {static_cast<const std::byte*>(data_), size_};
   }

private:
   void* data_ = nullptr;
   size_t size_ = 0;
   std::chrono::system_clock::time_point mtime_;
   Identity id_{};
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

   auto file = fs::weakly_canonical(base / fs::path(path), ec);
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

Task<void> respond(server::Response& response, unsigned status)
{
   if (auto [ec] = co_await response.submit(status, fields({{"Content-Length", 0}})); !ec)
      co_await response.write_eof();
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
      auto entry = std::make_shared<const CachedFile>(
         *resolved, std::move(*mapped), std::move(last_modified), content_type(*resolved));
      insert(request_path, entry);
      return entry;
   }

private:
   std::shared_ptr<const CachedFile> lookup(const std::string& key)
   {
      const std::lock_guard lock{mutex_};
      const auto it = entries_.find(key);
      if (it == entries_.end())
         return nullptr;
      lru_.splice(lru_.begin(), lru_, it->second.lru); // most recently used first
      return it->second.entry;
   }

   void insert(const std::string& key, std::shared_ptr<const CachedFile> entry)
   {
      const std::lock_guard lock{mutex_};

      if (const auto it = entries_.find(key); it != entries_.end())
      {
         bytes_ -= it->second.entry->file.size();
         lru_.erase(it->second.lru);
         entries_.erase(it);
      }

      bytes_ += entry->file.size();
      lru_.push_front(key);
      entries_.emplace(key, Slot{std::move(entry), lru_.begin()});

      //
      // Keep at least the entry just inserted, so a file larger than the byte budget still gets
      // served from the cache rather than being evicted immediately every time.
      //
      while (entries_.size() > 1 && (entries_.size() > max_entries || bytes_ > max_bytes))
      {
         const auto victim = entries_.find(lru_.back());
         bytes_ -= victim->second.entry->file.size();
         entries_.erase(victim);
         lru_.pop_back();
      }
   }

   struct Slot
   {
      std::shared_ptr<const CachedFile> entry;
      std::list<std::string>::iterator lru;
   };

   std::mutex mutex_;
   std::unordered_map<std::string, Slot> entries_;
   std::list<std::string> lru_;
   size_t bytes_ = 0;
};

FileCache g_cache;

} // namespace

namespace anyhttp
{

Task<void> serve_file(server::Request request, server::Response response, fs::path root,
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
      // answered with a 4xx, which is the client's problem rather than ours
      logi("[{}] serve_file: {}: {}", request.log_prefix(), path, entry.error().message());
      co_await respond(response, status_for(entry.error()));
      co_return;
   }

   const auto& file = (*entry)->file;
   logd("serve_file: {} ({} bytes)", (*entry)->resolved.native(), file.size());
   if (auto [ec] =
          co_await response.submit(200, fields({{"Content-Length", file.size()},
                                                {"Content-Type", (*entry)->content_type},
                                                {"Last-Modified", (*entry)->last_modified}}));
       ec)
      co_return;

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
   co_await response.write_eof(asio::buffer(file.bytes()));
}

} // namespace anyhttp
