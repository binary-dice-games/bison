# TLS-Secured Transport

`tls_socket_client_transport` / `tls_socket_server_transport`
(`src/rmi/transport/tls_socket_transport.hpp`) are TLS-encrypted siblings of
`socket_client_transport` / `socket_server_transport`: same three transport
interfaces, same framing (see [FORMAT.md](../FORMAT.md) §5.1/§5.1a), same
`server_transport.connect()`/`accept()` shape. Use them anywhere
`socket_*_transport` is used today when the connection crosses an untrusted
network.

By default the server proves its identity to the client with a certificate
(the same trust model as HTTPS); the client verifies it against a trust
anchor. Client identity is then established the usual bison way: the
application-level `auth_module_iface` hook
([src/rmi/server/auth.hpp](../src/rmi/server/auth.hpp)) runs over the
now-encrypted, server-authenticated channel via the ordinary `"connect"`
operation payload. Mutual TLS (verifying a client certificate at the
transport layer too) is available as an opt-in for deployments that want
machine-level client identity in addition to (or instead of)
`auth_module_iface` — see "Mutual TLS" below and
[src/rmi/DESIGN.md](../src/rmi/DESIGN.md) §13 for the tradeoff this defaults
against.

`--transport=tls` (and `--downstream_transport=tls`/`--upstream_transport=tls`
for `bridge_app`) is available on every `server_app`/`client_app`/`bridge_app`-based
binary bison ships (`bison-cli`, `calc-server`, `rmi_server_example`,
`rmi_client_example`, `rmi_bridge_example`) — see "CLI usage" below. It is
also available from the C ABI (`rmi_client_tls_create()` /
`rmi_server_tls_create()`) and every language binding built on it — see
"C ABI / language bindings" below.

## Generating a development certificate

For local testing, a self-signed CA and server certificate are enough:

```bash
openssl ecparam -name prime256v1 -genkey -noout -out ca-key.pem
openssl req -x509 -new -key ca-key.pem -days 3650 -out ca-cert.pem \
  -subj "/O=Dev/CN=Dev CA" -sha256

openssl ecparam -name prime256v1 -genkey -noout -out server-key.pem
openssl req -new -key server-key.pem -out server.csr -subj "/O=Dev/CN=localhost"
openssl x509 -req -in server.csr -CA ca-cert.pem -CAkey ca-key.pem -CAcreateserial \
  -out server-cert.pem -days 3650 -sha256 \
  -extfile <(printf "subjectAltName=IP:127.0.0.1,DNS:localhost")
```

The server's certificate's Subject Alternative Name must cover whatever
host/IP clients will connect to and verify against (`server_name` below) —
mbedTLS checks it during the handshake the same way a browser checks a
website's certificate.

For a real deployment, use certificates issued by your organization's CA (or
a public CA, for an internet-facing server) instead of a self-signed one.

## Server-only TLS (default)

```cpp
#include "src/rmi/transport/tls_socket_transport.hpp"
using namespace bdg::bison::rmi::transport;

tls_socket_server_transport server{"0.0.0.0", 8443};
bison::dynamic params;
params["cert_file"_key] = std::string{"server-cert.pem"};
params["key_file"_key] = std::string{"server-key.pem"};
server.start(params);
```

```cpp
tls_socket_client_transport client{"example.com", 8443};
bison::dynamic params;
params["ca_file"_key] = std::string{"ca-cert.pem"};
client.open(params);
```

`start()`/`open()` accept either a file path (`cert_file`/`key_file`/`ca_file`)
or an inline PEM string (`cert_pem`/`key_pem`/`ca_pem`) for each of these —
useful when certificate material comes from a secrets manager rather than
the filesystem.

### Server parameters (`start()`)

| Field | Type | Meaning |
|---|---|---|
| `cert_file` / `cert_pem` | string | Server certificate chain |
| `key_file` / `key_pem` | string | Server private key |
| `key_password` | string, optional | Passphrase for an encrypted private key |
| `client_auth` | string, default `"none"` | `"none"` \| `"optional"` \| `"required"` — see "Mutual TLS" |
| `ca_file` / `ca_pem` | string, required iff `client_auth != "none"` | Trust anchor for verifying client certificates |

### Client parameters (`open()`)

| Field | Type | Meaning |
|---|---|---|
| `server_name` | string, optional | SNI / hostname-verification target; defaults to `host` |
| `ca_file` / `ca_pem` | string | Trust anchor for verifying the server's certificate — required unless `insecure_skip_verify` is set |
| `insecure_skip_verify` | bool, default `false` | Skip server certificate verification entirely — **unsafe for production**, a `curl -k`-equivalent dev/test escape hatch for self-signed certs |
| `cert_file` / `cert_pem`, `key_file` / `key_pem` | string, optional | Client certificate/key, used only when the server requests/accepts mutual TLS |
| `key_password` | string, optional | Passphrase for an encrypted client private key |

## Mutual TLS

Set `client_auth` to `"optional"` or `"required"` on the server, and supply
`cert_file`/`key_file` (or `cert_pem`/`key_pem`) on the client:

```cpp
bison::dynamic server_params;
server_params["cert_file"_key] = std::string{"server-cert.pem"};
server_params["key_file"_key] = std::string{"server-key.pem"};
server_params["client_auth"_key] = std::string{"required"};
server_params["ca_file"_key] = std::string{"client-ca-cert.pem"}; // trust anchor for CLIENT certs
server.start(server_params);
```

```cpp
bison::dynamic client_params;
client_params["ca_file"_key] = std::string{"ca-cert.pem"};       // trust anchor for the SERVER's cert
client_params["cert_file"_key] = std::string{"client-cert.pem"};
client_params["key_file"_key] = std::string{"client-key.pem"};
client.open(client_params);
```

Note the server and client each have their *own* `ca_file`/`ca_pem`: the
server's verifies incoming client certificates, the client's verifies the
server's certificate. They are typically different CAs in a real deployment
(a public/internal CA for the server, an internal-only CA for a fixed fleet
of clients).

Reach for mutual TLS when clients are a small, fixed, operator-controlled
fleet (service-to-service calls, a zero-trust internal mesh) where issuing
and rotating a certificate per client is cheap. For an open or
product-facing client population, the default (server-only TLS plus
`auth_module_iface` for client login) is the better fit — see
[src/rmi/DESIGN.md](../src/rmi/DESIGN.md) §13 for the full reasoning.

## CLI usage

`--transport=tls` is a first-class value everywhere `--transport=tcp` works,
on `bison-cli`, `calc-server`, `rmi_server_example`, and `rmi_client_example`.
The `dynamic` param keys above are exposed 1:1 as flags (`cert_file` →
`--cert_file`, etc.); `--host`/`--port` are reused unchanged from `tcp`.

```bash
# Server-only TLS
./calc-server --transport=tls --host=0.0.0.0 --port=8443 \
    --cert_file=server-cert.pem --key_file=server-key.pem

./bison-cli --transport=tls --host=<server-host> --port=8443 \
    --ca_file=ca-cert.pem
```

```bash
# Mutual TLS
./calc-server --transport=tls --host=0.0.0.0 --port=8443 \
    --cert_file=server-cert.pem --key_file=server-key.pem \
    --client_auth=required --ca_file=client-ca-cert.pem

./bison-cli --transport=tls --host=<server-host> --port=8443 \
    --ca_file=ca-cert.pem --cert_file=client-cert.pem --key_file=client-key.pem
```

`rmi_server_example`/`rmi_client_example` accept the identical flags (see
[docs/examples.md](examples.md)).

### Bridge (`bridge_app`)

`bridge_app`'s two sides (`rmi_bridge_example`) use the same
`downstream_`/`upstream_`-prefixed convention as its other flags
(`--downstream_host`, `--upstream_host`, etc.):

```bash
./rmi_bridge_example \
    --downstream_transport=tls --downstream_host=0.0.0.0 --downstream_port=8443 \
    --downstream_cert_file=server-cert.pem --downstream_key_file=server-key.pem \
    --upstream_transport=tls --upstream_host=<upstream-host> --upstream_port=8444 \
    --upstream_ca_file=ca-cert.pem
```

`--downstream_client_auth`/`--downstream_ca_file` (mutual TLS on the
downstream/listening side) and `--upstream_cert_file`/`--upstream_key_file`
(mutual TLS on the upstream/dial-out side) mirror the server/client flags
above, `downstream_`/`upstream_`-prefixed.

## C ABI / language bindings

`rmi_client_tls_create(host, port)` / `rmi_server_tls_create(host, port)`
(`include/rmi_c.h`) are the TLS counterparts of `rmi_client_tcp_create()` /
`rmi_server_tcp_create()`. The client/server parameter tables above apply
unchanged, supplied via `rmi_client_connect()`'s / `rmi_server_listen()`'s
existing `params` argument (a `bison_handle`) — there is no separate TLS-only
entry point for configuring trust/identity material.

```c
bison_handle params = bison_create(0);
bison_set_string(params, bison_key("ca_file"), "ca-cert.pem");

rmi_client_handle client = rmi_client_tls_create("example.com", 8443);
rmi_client_connect(client, params);
```

```c
bison_handle params = bison_create(0);
bison_set_string(params, bison_key("cert_file"), "server-cert.pem");
bison_set_string(params, bison_key("key_file"), "server-key.pem");

rmi_server_handle server = rmi_server_tls_create("0.0.0.0", 8443);
rmi_server_listen(server, params, NULL, NULL);
```

Every language binding built on `rmi_c.h` exposes the same factory
alongside `tcp()`/`pipe()`/`term()`: `client::tls()` / `server::tls()`
(C++, `bindings/cpp`), `Client.tls()` / `Server.tls()` (Python), and
`Client.Tls()` / `Server.Tls()` (C#) — see [docs/bindings.md](bindings.md).

## Why mbedTLS

See [src/rmi/DESIGN.md](../src/rmi/DESIGN.md) §3.4 for the library
comparison (mbedTLS vs. OpenSSL/BoringSSL/wolfSSL) and why mbedTLS fits
bison's static-linking, cross-platform (Linux/MSYS2/native-Windows) build
requirements best.
