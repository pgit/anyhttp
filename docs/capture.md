# Capturing traffic

How to look at what the server and its clients actually put on the wire, TLS and QUIC included:
`tcpdump` captures, `tshark` decodes, and a key log written by the client lets it decrypt.

## Capture

```
sudo tcpdump -i lo -U -w ssl.pcap 'port 18080'
```

- `port 18080` matches TCP and UDP, so HTTP/1.1, HTTP/2 and HTTP/3 alike.
- Start it before the client connects, and wait for `listening on lo`: decryption needs the
  handshake. Stop it with Ctrl-C.
- tcpdump drops root before it writes, so the file belongs to the user `tcpdump`. `*.pcap` and
  `keylog.log` are in `.gitignore`.
- In a script, add `--immediate-mode`. Otherwise libpcap hands packets over in batches, about a
  second apart, and stopping tcpdump right after the client has finished loses the last batch: "62
  packets received by filter, 0 packets captured".
- For a live view, pipe it: `sudo tcpdump -i lo -U -w - 'port 18080' | tshark -r -`.

Plain HTTP/2 needs no options: tshark recognizes the connection preface (`Magic`). Only a capture
that starts in the middle of a connection needs `-d tcp.port==18080,http2` -- and that option then
breaks TLS on the same port, which the server also accepts.

## Decrypting TLS and QUIC

Have the client write its session secrets to a key log (the NSS format), and give that to tshark:

```
export SSLKEYLOGFILE=keylog.log
sudo tcpdump -i lo -U -w ssl.pcap 'port 18080'          # in a second terminal, until done
curl -v --cacert pki/out/root.pem https://localhost:18080/test/data/64kminus1 -o /dev/null
tshark -r ssl.pcap -o tls.keylog_file:keylog.log
```

- curl, h2load and nghttp honour `SSLKEYLOGFILE`, and so do browsers and Python's
  `ssl.create_default_context()`. Our own client and server do not write a key log.
- The same key log decrypts HTTP/3: `curl --http3-only ...`.
- The client appends to the file, and secrets of earlier sessions do no harm. But `export` makes
  every TLS client started from that shell write its secrets there: `unset SSLKEYLOGFILE`
  afterwards, or set it for the one command only (`SSLKEYLOGFILE=keylog.log curl ...`).
- Live decryption works too: tshark reads the key log again whenever it needs keys it does not
  have, so `... | tshark -r - -o tls.keylog_file:keylog.log` picks up a session that starts after
  it.

## Useful views

The one-line summary of a frame carrying HTTP/2 or TLS does not show its TCP flags, so a FIN that
rides on the last data segment is easy to miss. To list them:

```
tshark -r ssl.pcap -o tls.keylog_file:keylog.log \
  -T fields -e frame.number -e tcp.srcport -e tcp.flags.str -e _ws.col.Info
```

Narrower views, each added to the `tshark -r ssl.pcap -o tls.keylog_file:keylog.log` above:

- TLS alerts, close_notify among them: `-Y tls.alert_message`
- GOAWAY frames, with their last stream ID and error code:
  `-Y 'http2.type == 7' -T fields -e frame.number -e tcp.srcport -e http2.goaway.last_stream_id -e http2.goaway.error`
- One frame in full: `-Y frame.number==25 -O http2 -V`

## Without a key log

TLS 1.3 encrypts the record type, so an alert looks like `Application Data`. Its size gives it away
all the same: 2 bytes of alert, 1 byte of inner type and a 16-byte AEAD tag make a record of length
19, 24 bytes on the wire. An HTTP/2 frame alone is 9 bytes, so no record carrying HTTP/2 is that
short:

```
tshark -r ssl.pcap -Y 'tls.record.length == 19'
```

That tells an alert from data, not which alert it is -- but the last one before a FIN is the
close_notify. It does not hold for a peer that pads its records (OpenSSL and AWS-LC do not by
default).

## What a clean end looks like

Each side sends its GOAWAY, its close_notify (TLS only) and its FIN, and keeps reading until the
peer's FIN before it closes the socket. The server does that (`ServerSession::do_session()`), and
with a client that does the same, a TLS connection ends:

```
client  GOAWAY, close_notify, FIN
server  GOAWAY(last_stream_id=1, NO_ERROR)
server  close_notify
server  FIN
client  ACK
```

Many clients do not wait: h2load, curl and nghttp close their socket right after their own GOAWAY
and close_notify. Whatever the server still sends -- its GOAWAY, its close_notify -- then reaches a
closed socket, and the client's kernel answers it with an RST. That RST is the client's doing; it
happens with nghttpd as well.
