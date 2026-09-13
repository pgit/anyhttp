#include "test_fixtures.hpp"

#include "anyhttp/file_handler.hpp"

#include <filesystem>
#include <fstream>

// =================================================================================================

//
// serve_file() mounted on "/custom", serving a directory tree created fresh for each testcase.
//
class FileHandler : public ClientAsync
{
public:
   void SetUp() override
   {
      base = std::filesystem::temp_directory_path() /
             std::format("anyhttp-file-handler-{}", ::getpid());
      root = base / "docroot";
      std::filesystem::remove_all(base);
      std::filesystem::create_directories(root / "sub");

      write(root / "hello.txt", "Hello, File!");
      write(root / "empty.txt", "");
      write(root / "sub" / "nested.txt", "Nested!");
      write(root / "large.bin", std::string(256_k, 'x'));
      write(root / "secret.txt", "no peeking");
      write(root / "er.txt", "leaked"); // what "/customer.txt" resolves to without a segment check
      std::filesystem::permissions(root / "secret.txt", std::filesystem::perms::none);
      std::filesystem::create_symlink(base / "outside.txt", root / "escape.txt");

      // just outside the root, reachable only by escaping it -- so a missing check shows up as
      // content served instead of a 404
      write(base / "outside.txt", "outside");

      ClientAsync::SetUp();

      custom = [this](server::Request request, server::Response response) -> awaitable<void> {
         co_await serve_file(std::move(request), std::move(response), root, "/custom");
      };
   }

   void TearDown() override
   {
      ClientAsync::TearDown();
      std::filesystem::remove_all(base);
   }

   static void write(const std::filesystem::path& path, std::string_view content)
   {
      std::ofstream(path, std::ios::binary).write(content.data(), content.size());
   }

   //
   // Requests \p target and returns status code and body. The request is finished right away --
   // serve_file() ignores the request body, but still has to consume it.
   //
   awaitable<std::tuple<int, std::string>> get(Session& session, boost::urls::url target)
   {
      auto request = co_await session.async_submit(target, {});
      co_await request.async_write_eof();
      auto response = co_await request.async_get_response();
      auto body = co_await read(response);
      co_return std::make_tuple(response.status_code(), std::move(body));
   }

   awaitable<std::tuple<int, std::string>> get(Session& session, std::string_view path)
   {
      co_return co_await get(session, boost::urls::url(url).set_path(path));
   }

   /// Target with a percent-encoded path, passed to the server as-is.
   boost::urls::url encoded(std::string_view path) const
   {
      auto target = boost::urls::url(url);
      target.set_encoded_path(path);
      return target;
   }

protected:
   std::filesystem::path base; ///< holds the docroot and the file just outside of it
   std::filesystem::path root; ///< what serve_file() is mounted on
};

// -------------------------------------------------------------------------------------------------

INSTANTIATE_TEST_SUITE_P(FileHandler, FileHandler,
                         ::testing::Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2,
                                           anyhttp::Protocol::h3),
                         NameGenerator);

// =================================================================================================

TEST_P(FileHandler, WHEN_file_exists_THEN_serves_content)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/custom/hello.txt");
      EXPECT_EQ(status, 200);
      EXPECT_EQ(body, "Hello, File!");
   };
}

TEST_P(FileHandler, WHEN_file_is_in_subdirectory_THEN_serves_content)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/custom/sub/nested.txt");
      EXPECT_EQ(status, 200);
      EXPECT_EQ(body, "Nested!");
   };
}

//
// An empty file cannot be mmap()ed at all, and must not send the empty buffer that already means
// EOF twice.
//
TEST_P(FileHandler, WHEN_file_is_empty_THEN_serves_empty_body)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/custom/empty.txt");
      EXPECT_EQ(status, 200);
      EXPECT_EQ(body, "");
   };
}

//
// Large enough that a single async_write() spans many QUIC packets, so the HTTP/3 write path has
// to keep handing out the caller's buffer across several write_pkt() rounds.
//
TEST_P(FileHandler, WHEN_file_is_large_THEN_serves_all_of_it)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/custom/large.bin");
      EXPECT_EQ(status, 200);
      EXPECT_EQ(body, std::string(256_k, 'x'));
   };
}

TEST_P(FileHandler, WHEN_file_does_not_exist_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/custom/missing.txt");
      EXPECT_EQ(status, 404);
      EXPECT_EQ(body, "");
   };
}

//
// A directory can be open()ed but not mapped, and we do not serve listings.
//
TEST_P(FileHandler, WHEN_path_is_a_directory_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      EXPECT_EQ(std::get<0>(co_await get(session, "/custom/sub")), 404);
      EXPECT_EQ(std::get<0>(co_await get(session, "/custom/")), 404);
   };
}

TEST_P(FileHandler, WHEN_path_escapes_the_root_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      EXPECT_EQ(std::get<0>(co_await get(session, "/custom/../outside.txt")), 404);
      EXPECT_EQ(std::get<0>(co_await get(session, "/custom/sub/../../outside.txt")), 404);
      EXPECT_EQ(std::get<0>(co_await get(session, encoded("/custom/%2e%2e/outside.txt"))), 404);
   };
}

//
// weakly_canonical() resolves the link, so a link out of the root is caught like any other escape.
//
TEST_P(FileHandler, WHEN_symlink_points_outside_the_root_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      EXPECT_EQ(std::get<0>(co_await get(session, "/custom/escape.txt")), 404);
   };
}

//
// The mount prefix must match whole path segments, not just any leading characters: stripping
// "/custom" off "/customer.txt" would otherwise serve "er.txt" out of the docroot.
//
TEST_P(FileHandler, WHEN_prefix_matches_mid_segment_THEN_error_404)
{
   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/customer.txt");
      EXPECT_EQ(status, 404);
      EXPECT_EQ(body, "");
      EXPECT_EQ(std::get<0>(co_await get(session, "/customer/hello.txt")), 404);
   };
}

TEST_P(FileHandler, WHEN_file_is_not_readable_THEN_error_403)
{
   if (::geteuid() == 0)
      GTEST_SKIP() << "running as root, permissions do not apply";

   test = [this](Session session) -> awaitable<void>
   {
      auto [status, body] = co_await get(session, "/custom/secret.txt");
      EXPECT_EQ(status, 403);
      EXPECT_EQ(body, "");
   };
}

TEST_P(FileHandler, WHEN_same_file_is_requested_twice_THEN_serves_it_twice)
{
   test = [this](Session session) -> awaitable<void>
   {
      EXPECT_EQ(std::get<1>(co_await get(session, "/custom/hello.txt")), "Hello, File!");
      EXPECT_EQ(std::get<1>(co_await get(session, "/custom/hello.txt")), "Hello, File!");
   };
}

// =================================================================================================
