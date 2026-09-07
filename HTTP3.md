# HTTP/3 Manual Testing

`osslclient` and `osslserver` are the [ngtcp2](https://github.com/ngtcp2/ngtcp2) example
client and server; the devcontainer image builds them from upstream and installs them as
`/usr/local/bin/osslclient` and `/usr/local/bin/osslserver`. A few small helpers from the
same examples live in `anyhttp/h3_common.hpp`.

```sh
osslserver ::1 8080 pki/out/server-key.pem pki/out/server-chain.pem
```

```sh
curl --http3-only --cacert pki/out/root.pem https://[::1]:8080/CMakeLists.txt -vv
```
