## Server
```bash
build/src/ngtcp2/ngtcp-server ::1 8080 pki/out/server-key.pem pki/out/server.pem
```

## Client
```bash
export SSLKEYLOGFILE=secrets
build/src/ngtcp2/ngtcp-client ::1 8080 'https://[::1]:8080/README.md
```
