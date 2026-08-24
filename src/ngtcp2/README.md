# ngtcp2 example helpers

A few small helpers extracted from the [ngtcp2](https://github.com/ngtcp2/ngtcp2) examples
(`examples/`, v1.25.0), reduced to what the anyhttp library actually uses:

* `network.h` -- `sockaddr_union` and `Address`
* `util.{h,cc}` -- `format_hex()`, `timestamp()`, `straddr()`
* `shared.{h,cc}` -- `msghdr_get_local_addr()`, `set_port()`

The full example client and server used to be vendored here as `ngtcp-client`/`ngtcp-server`.
They are gone; the devcontainer image builds the upstream examples instead and installs them
as `/usr/local/bin/osslclient` and `/usr/local/bin/osslserver` -- see [HTTP3.md](../../HTTP3.md).
