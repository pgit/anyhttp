# HTTP/3 Manual Testing

```sh
cmake --build build && build/src/ngtcp2/ngtcp-server ::1 8080 pki/out/server-key.pem pki/out/server-chain.pem
```

```sh
curl --http3-only --cacert pki/out/root.pem https://[::1]:8080/CMakeLists.txt -vv
```
