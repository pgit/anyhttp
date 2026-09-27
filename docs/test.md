# Unit tests

The tests live in [`test/`](../test) and share their fixtures through
[`test_fixtures.hpp`](../test/test_fixtures.hpp). Two ideas carry most of the suite: every
client/server test runs once per protocol, and an asynchronous test is written as two coroutines,
one for the server and one for the client, which the fixture then runs to completion.

## One test, three protocols

The fixtures are parametrized on `anyhttp::Protocol`. The server accepts all protocols at runtime
(HTTP/1.1 and HTTP/2 over TCP, HTTP/3 over UDP on the same port), so the parameter only selects
what the *client* speaks:

```
Server            testing::TestWithParam<Protocol>; server on 127.0.0.2, ephemeral port
└── Client        adds a client::Client, configured with .protocol = GetParam()
    └── ClientAsync   runs the test as coroutines, see below
```

A test suite instantiates the fixture for the protocols it covers:

```cpp
INSTANTIATE_TEST_SUITE_P(ClientAsync, ClientAsync,
                         Values(anyhttp::Protocol::http11, anyhttp::Protocol::h2,
                                anyhttp::Protocol::h3),
                         NameGenerator);
```

`NameGenerator` names the instances `/HTTP11`, `/HTTP2` and `/HTTP3`, so a single protocol can be
selected with `--gtest_filter='*/HTTP3'`. A test body can branch on `GetParam()` where the
protocols genuinely differ, typically to skip:

```cpp
if (GetParam() == anyhttp::Protocol::http11)
   GTEST_SKIP(); // a chunked body cannot be cancelled correctly --> disconnects
```

Protocol-specific suites (for example the HTTP/2 `AltSvcFrame` tests) instantiate with a single
value instead.

A derived fixture adjusts the configuration by overriding `configure_server()` or
`configure_client()`, which are called before the server and client are created:

```cpp
class HeaderLimits : public ClientAsync
{
protected:
   void configure_server(server::Config& config) override { config.max_header_size = 4_k; }
   void configure_client(client::Config& config) override { config.max_header_size = 4_k; }
};
```

## Asynchronous tests: `requestHandler` and `clientSession`

A `ClientAsync` test body does not do any I/O itself. It only assigns up to two coroutines:

- **`requestHandler`** -- the server side. The fixture's server routes a few fixed paths to
  built-in handlers (`/echo`, `/eat_request`, `/discard`, `/detach`, `/dump`, ...) and everything
  under `/custom` to `requestHandler`. The client's default URL is `http://127.0.0.2/custom`, so a
  test that sets `requestHandler` and submits to `url` reaches it without further ado.
- **`clientSession`** -- the client side. It receives a connected `Session` and runs the test
  scenario against it.

```cpp
TEST_P(ClientAsync, WHEN_server_discards_request_and_response_THEN_completes_anyway)
{
   requestHandler = [this](server::Request request, server::Response response) -> awaitable<void> {
      co_return; // drops both
   };
   clientSession = [this](Session session) -> awaitable<void> {
      auto request = co_await session.async_submit(url);
      auto [ec, _] = co_await request.async_get_response(as_tuple);
      EXPECT_EQ(ec, boost::beast::http::error::end_of_stream);
   };
}
```

Tests that only need the built-in handlers leave `requestHandler` unset and change the path, e.g.
`session.async_submit(url.set_path("echo"), {})`.

### How it runs

1. `SetUp()` creates server and client and `co_spawn`s the test coroutine on the client's
   executor. Nothing runs yet -- the `io_context` has not been started.
2. The test body assigns `requestHandler` and `clientSession`.
3. `TearDown()` runs the `io_context`. The spawned coroutine connects, calls `clientSession`, and
   the server calls `requestHandler` for each request under `/custom`.
4. When `clientSession` finishes, the completion token calls `on_complete(ec)`, destroys the
   server and releases the work guard, so `run()` returns once everything has shut down.

`on_complete` is a gmock method, and `TearDown()` expects it to be called exactly once with
success. A `clientSession` that throws -- for example because an operation without `as_tuple`
failed -- therefore fails the test, as does one that never finishes (it hangs, in practice). To
test for an error, catch it with `as_tuple` and check it with `EXPECT_*` inside the coroutine;
by default the fixture is single-threaded, so gtest assertions work there as usual.

The helpers in [`request_handlers.hpp`](../include/anyhttp/request_handlers.hpp) (`generate`,
`send`, `drain`, `count_response`, `sleep`, ...) keep both coroutines short, and composing them
with `&&` runs the upload and the download concurrently:

```cpp
auto count = co_await (generate(request, bytes) && count_response(request));
```
