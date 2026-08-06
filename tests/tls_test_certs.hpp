// MIT License © 2025 Binary Dice Games
/**
 * @file tls_test_certs.hpp
 * @brief Throwaway self-signed test PKI for tls_socket_transport_tests.cpp
 *        and the RmiAuthOverTls tests in rmi_tests.cpp.
 *
 * Generated once with:
 *   openssl ecparam -name prime256v1 -genkey -noout -out ca-key.pem
 *   openssl req -x509 -new -key ca-key.pem -days 3650 -out ca-cert.pem \
 *     -subj "/O=Bison Test/CN=Bison Test CA" -sha256
 *   openssl ecparam -name prime256v1 -genkey -noout -out server-key.pem
 *   openssl req -new -key server-key.pem -out server.csr \
 *     -subj "/O=Bison Test/CN=127.0.0.1"
 *   openssl x509 -req -in server.csr -CA ca-cert.pem -CAkey ca-key.pem \
 *     -CAcreateserial -out server-cert.pem -days 3650 -sha256 \
 *     -extfile <(printf "subjectAltName=IP:127.0.0.1,DNS:localhost")
 *   # client-{key,cert}.pem: same as server, CN=bison-test-client, no SAN
 *   # untrusted-{key,cert}.pem: self-signed (NOT signed by ca-key.pem),
 *   #   same CN/SAN as server-cert.pem, used to prove clients reject a
 *   #   server presenting a cert outside their trusted CA.
 *
 * These keys/certs are test-only fixtures with no confidentiality value --
 * committing them is intentional (avoids depending on an `openssl` CLI
 * being present in CI just to run the test suite).
 */
#pragma once

#include <string>

namespace bdg::bison::rmi::transport::test {

inline const std::string kTestCaCert = R"(-----BEGIN CERTIFICATE-----
MIIBrzCCAVWgAwIBAgIUQyEsrZisez6tX0QJEFe/4L63MNowCgYIKoZIzj0EAwIw
LTETMBEGA1UECgwKQmlzb24gVGVzdDEWMBQGA1UEAwwNQmlzb24gVGVzdCBDQTAe
Fw0yNjA4MDYwOTIzMjRaFw0zNjA4MDMwOTIzMjRaMC0xEzARBgNVBAoMCkJpc29u
IFRlc3QxFjAUBgNVBAMMDUJpc29uIFRlc3QgQ0EwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAAQdQwpEDnBZXC7j6ZmXMKNDFKSrD4QjG1hfxe0+y4pmIuiyUJ+ce+bl
8GDY3awxaTG5bIdmSiZtqnkq8fGpmdvmo1MwUTAdBgNVHQ4EFgQUQkDMwJ9SfWEe
FgVGbdHMBd9hNdkwHwYDVR0jBBgwFoAUQkDMwJ9SfWEeFgVGbdHMBd9hNdkwDwYD
VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNIADBFAiAPFCtwVgWSpB3JmvrZdPHT
9Iw7c9CT2S1PpGCPp0yZKwIhAM+kud5IWOXTQG7ClYm++bxTlxVwzG/Slrx7e5Qr
Wmkj
-----END CERTIFICATE-----
)";

/** @brief Server cert signed by kTestCaCert. CN=127.0.0.1, SAN=IP:127.0.0.1,DNS:localhost. */
inline const std::string kTestServerCert = R"(-----BEGIN CERTIFICATE-----
MIIBtTCCAVygAwIBAgIUJqr/3DEiE+yXt7/Qz+IC61RZPzwwCgYIKoZIzj0EAwIw
LTETMBEGA1UECgwKQmlzb24gVGVzdDEWMBQGA1UEAwwNQmlzb24gVGVzdCBDQTAe
Fw0yNjA4MDYwOTIzMjRaFw0zNjA4MDMwOTIzMjRaMCkxEzARBgNVBAoMCkJpc29u
IFRlc3QxEjAQBgNVBAMMCTEyNy4wLjAuMTBZMBMGByqGSM49AgEGCCqGSM49AwEH
A0IABEBeWCQdI2vJuzr7N4uCDPz89McV0W7Wj6pHIVYJxXmvtcbY8lX9AnD20lhD
kWdzvWYh5Y8CXGaQx/dTj68uI3WjXjBcMBoGA1UdEQQTMBGHBH8AAAGCCWxvY2Fs
aG9zdDAdBgNVHQ4EFgQUGtY0xSXmOxHQSBnFUEiYVKMIpLYwHwYDVR0jBBgwFoAU
QkDMwJ9SfWEeFgVGbdHMBd9hNdkwCgYIKoZIzj0EAwIDRwAwRAIgHE268rWKST8z
sZrRrjSIsYSz2DmW1BW1cJnJSzgpZOECIDQfUauC6aMbsOVrUfCAWNjp9q+QYkt0
eszXzv891gzf
-----END CERTIFICATE-----
)";

inline const std::string kTestServerKey = R"(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIMPIZN1JRrX9xjwyca5+XlsaHUG5nAbvwRU6n/qq6/tJoAoGCCqGSM49
AwEHoUQDQgAEQF5YJB0ja8m7Ovs3i4IM/Pz0xxXRbtaPqkchVgnFea+1xtjyVf0C
cPbSWEORZ3O9ZiHljwJcZpDH91OPry4jdQ==
-----END EC PRIVATE KEY-----
)";

/** @brief Client cert signed by kTestCaCert, for mutual-TLS test cases. */
inline const std::string kTestClientCert = R"(-----BEGIN CERTIFICATE-----
MIIBWTCB/wIUJqr/3DEiE+yXt7/Qz+IC61RZPz0wCgYIKoZIzj0EAwIwLTETMBEG
A1UECgwKQmlzb24gVGVzdDEWMBQGA1UEAwwNQmlzb24gVGVzdCBDQTAeFw0yNjA4
MDYwOTIzMjRaFw0zNjA4MDMwOTIzMjRaMDExEzARBgNVBAoMCkJpc29uIFRlc3Qx
GjAYBgNVBAMMEWJpc29uLXRlc3QtY2xpZW50MFkwEwYHKoZIzj0CAQYIKoZIzj0D
AQcDQgAEblkIrPnNG/joqLiC2rvgcTGdXOF9X36DNT/wK2tiGMLEEq6YR2G8YoeH
zmqKodbRdKh9DVd/tr3V4QerZUNw3DAKBggqhkjOPQQDAgNJADBGAiEAxZoZnSud
tkOaMz9MGHcMbtOMIwL0E0gFjFQz5QepTYoCIQCOqqA+ccK0bnh9sU26892fokT1
SjMm7OJsFzSUsWGUnA==
-----END CERTIFICATE-----
)";

inline const std::string kTestClientKey = R"(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIFQzqnMa4Kms42xHVNXUBE6SmzLXCy2vo2GXZ+UBehe1oAoGCCqGSM49
AwEHoUQDQgAEblkIrPnNG/joqLiC2rvgcTGdXOF9X36DNT/wK2tiGMLEEq6YR2G8
YoeHzmqKodbRdKh9DVd/tr3V4QerZUNw3A==
-----END EC PRIVATE KEY-----
)";

/**
 * @brief Self-signed server cert NOT signed by kTestCaCert (same CN/SAN as
 *        kTestServerCert). Used to prove a client verifying against
 *        kTestCaCert rejects an otherwise-plausible-looking server cert
 *        from an untrusted issuer.
 */
inline const std::string kUntrustedServerCert = R"(-----BEGIN CERTIFICATE-----
MIIBwjCCAWegAwIBAgIUO7CBEQ136kUbCtFR0enQ42j0Bt8wCgYIKoZIzj0EAwIw
KDESMBAGA1UECgwJVW50cnVzdGVkMRIwEAYDVQQDDAkxMjcuMC4wLjEwHhcNMjYw
ODA2MDkyMzI0WhcNMzYwODAzMDkyMzI0WjAoMRIwEAYDVQQKDAlVbnRydXN0ZWQx
EjAQBgNVBAMMCTEyNy4wLjAuMTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABK2z
1b3CLtoa3sJsfcs41lI5gyQMVaMOFGyvzlYh0kSYlBmIhg7ZC8LqBx0p5JFWYQWd
qBcxgwu9eWz933TJkAKjbzBtMB0GA1UdDgQWBBRGy4WpAef2GFGdpwUIXqIm67Sz
LzAfBgNVHSMEGDAWgBRGy4WpAef2GFGdpwUIXqIm67SzLzAPBgNVHRMBAf8EBTAD
AQH/MBoGA1UdEQQTMBGHBH8AAAGCCWxvY2FsaG9zdDAKBggqhkjOPQQDAgNJADBG
AiEArTIQv27xe/tXTrwjnNdtUVH/5xPVNsr+5xPPuir9su4CIQDQ5ljxRtWXin66
DPrTzZVkYifcaq3WQf2Z04PP4XJpTw==
-----END CERTIFICATE-----
)";

inline const std::string kUntrustedServerKey = R"(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIBagcJm8k01MX7WVmF1j8E0ZPezGb3OlqvlzP/zCTCCsoAoGCCqGSM49
AwEHoUQDQgAErbPVvcIu2hrewmx9yzjWUjmDJAxVow4UbK/OViHSRJiUGYiGDtkL
wuoHHSnkkVZhBZ2oFzGDC715bP3fdMmQAg==
-----END EC PRIVATE KEY-----
)";

} // namespace bdg::bison::rmi::transport::test
