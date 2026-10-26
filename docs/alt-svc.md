# Moving to HTTP/3: Alt-Svc

HTTP/3 runs on QUIC, and QUIC is not something a TCP connection can turn into: there is no
`Connection: Upgrade` on the way to HTTP/3, the way there is one from HTTP/1.1 to HTTP/2. What
there is instead is the server saying where else it can be reached
([RFC 7838](https://www.rfc-editor.org/rfc/rfc7838)), and the client making its *next* connection
there.

The server does that for its own HTTP/3 endpoint, which shares the address and port the TCP
acceptor is listening on, so the advertised alt-authority is a port and nothing else -- an empty
host in one means "the host of the origin":

```
Alt-Svc: h3=":8080"; ma=86400
```

It goes into every response sent over HTTP/1.1 and HTTP/2, but never over HTTP/3, which is already
there. `server::Config::alt_svc_max_age` is how long a client may remember it, and `0s` advertises
nothing at all.

A client only acts on it with `client::Config::follow_alt_svc` set, and then it takes precedence
over `client::Config::protocol`:

```c++
   client::Client client(executor, {.url = url, .protocol = Protocol::h2, .follow_alt_svc = true});

   auto [ec1, first] = co_await client.connect();  // HTTP/2, and learns about the alternative
   auto [ec2, second] = co_await client.connect(); // HTTP/3
```

The session that learns about the alternative keeps speaking what it speaks -- a connection in the
middle of a request can not be moved -- and the alternative is remembered for as long as `ma` says,
but only for the lifetime of the `Client`: there is no cache on disk. The origin does not change
with any of this, only where it is reached: requests still go out with the authority of
`Config::url`.

Over HTTP/2, an alternative may also arrive in an `ALTSVC` frame instead of a header field, which
lets a server advertise before the first request has even been sent. anyhttp's client reads both;
its server sends the header field only.

`curl` does the same thing, which is the easy way to watch it happen -- it honours `Alt-Svc` for
`https://` origins only, so this needs TLS, and a cache file to remember the alternative between
invocations:

```sh
./build/src/server -p 8080
```

```sh
curl --alt-svc altsvc.txt --cacert pki/out/root.pem https://localhost:8080/echo -d hello -so/dev/null -w '%{http_version}\n'
```

The first run answers `2`, and every one after it `3`.
