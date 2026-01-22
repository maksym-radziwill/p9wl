/*
 * p9_tls.h - TLS support for 9P connections
 *
 * Provides TLS transport with certificate pinning for secure
 * connections to Plan 9 systems.
 *
 * See p9_tls.c for detailed usage instructions and 9front setup.
 *
 * NOTE: 9front's tlssrv requires PEM format certificates.
 * Generate with: auth/rsa2x509 ... | auth/pemencode CERTIFICATE > cert
 */

#ifndef P9_TLS_H
#define P9_TLS_H

#include <stdint.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

/* Default port for 9front TLS connections */
#define P9_TLS_PORT 10001

/* TLS configuration */
struct tls_config {
    char *cert_file;           /* Path to pinned certificate (PEM format) */
    char *cert_fingerprint;    /* Alternative: SHA256 fingerprint (hex string) */
    int insecure;              /* Skip certificate verification (DANGEROUS) */
};

/* Initialize OpenSSL - call once at startup */
int tls_init(void);

/* Cleanup OpenSSL - call at shutdown */
void tls_cleanup(void);

/* Create TLS-wrapped connection over existing socket fd
 * Returns 0 on success, -1 on failure
 * On success, *ssl_out is set to the SSL object
 */
int tls_connect(int fd, SSL **ssl_out, struct tls_config *cfg);

/* Disconnect and free TLS resources */
void tls_disconnect(SSL *ssl);

/* Verify server certificate against pinned cert/fingerprint
 * Returns 0 if verification passes, -1 on failure
 */
int tls_verify_pinned(SSL *ssl, struct tls_config *cfg);

/* Read exactly n bytes through TLS connection
 * Returns n on success, -1 on error
 */
int tls_read_full(SSL *ssl, uint8_t *buf, int n);

/* Write exactly len bytes through TLS connection
 * Returns len on success, -1 on error
 */
int tls_write_full(SSL *ssl, const uint8_t *buf, int len);

/* Utility: compute SHA256 fingerprint of a PEM certificate file
 * Writes hex string to out (must be at least 65 bytes)
 * Returns 0 on success, -1 on error
 */
int tls_cert_file_fingerprint(const char *path, char *out, int outlen);

#endif /* P9_TLS_H */
