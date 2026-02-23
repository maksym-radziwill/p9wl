/*
 * p9_tls.h - TLS support for 9P connections
 *
 * Provides TLS transport with certificate pinning for secure connections
 * to Plan 9 systems. Uses OpenSSL for cryptographic operations.
 *
 * Security Model:
 *
 *   Traditional TLS relies on certificate authorities (CAs) to verify
 *   server identity. This module instead uses certificate pinning -
 *   the client has a copy of the expected server certificate and
 *   rejects connections if the certificate doesn't match.
 *
 *   This is more secure than CA-based verification for known servers
 *   and simpler to deploy (no CA infrastructure needed).
 *
 * Certificate Pinning Methods:
 *
 *   1. Certificate file: Provide path to server's PEM certificate.
 *      The entire certificate is compared byte-for-byte.
 *
 *   2. Fingerprint: Provide SHA256 hash of certificate as hex string.
 *      More portable (can be shared as a single string).
 *
 *   3. Insecure mode: Skip verification entirely (DANGEROUS).
 *      Only use for testing or initial fingerprint discovery.
 *
 * 9front Server Setup:
 *
 *   On 9front, use tlssrv to wrap a 9P server with TLS:
 *
 *     # Generate RSA key and self-signed certificate
 *     auth/rsagen -b 2048 > /sys/lib/tls/key
 *     auth/rsa2x509 'C=US CN=myserver' /sys/lib/tls/key |
 *         auth/pemencode CERTIFICATE > /sys/lib/tls/cert
 *
 *     # Start TLS-wrapped exportfs on port 10001
 *     tlssrv -c /sys/lib/tls/cert -l /net/tcp!*!10001 \
 *         exportfs -r /
 *
 *   Copy the cert file to the client for pinning.
 *
 * Certificate Format:
 *
 *   Certificates MUST be in PEM format (Base64 with headers):
 *
 *     -----BEGIN CERTIFICATE-----
 *     MIIC...base64 data...
 *     -----END CERTIFICATE-----
 *
 *   9front's auth/rsa2x509 outputs DER format by default. Pipe through
 *   auth/pemencode to convert:
 *
 *     auth/rsa2x509 ... | auth/pemencode CERTIFICATE > cert.pem
 *
 * Thread Safety:
 *
 *   - tls_init() must be called once before any other functions.
 *   - After init, tls_connect() can be called from multiple threads.
 *   - Each SSL object should only be used by one thread at a time.
 *   - tls_cleanup() should only be called during shutdown.
 *
 * OpenSSL Version:
 *
 *   Requires OpenSSL 1.1.0 or later. TLS 1.2 is the minimum protocol
 *   version; TLS 1.3 is preferred when supported by both sides.
 *
 * Usage:
 *
 *   // Initialize once at startup
 *   tls_init();
 *
 *   // Option 1: Pin by certificate file
 *   struct tls_config cfg = { .cert_file = "/path/to/server.pem" };
 *
 *   // Option 2: Pin by fingerprint
 *   struct tls_config cfg = {
 *       .cert_fingerprint = "a1b2c3d4..."  // 64 hex chars
 *   };
 *
 *   // Option 3: Insecure (for testing only!)
 *   struct tls_config cfg = { .insecure = 1 };
 *
 *   // Connect (fd is already-connected TCP socket)
 *   SSL *ssl;
 *   if (tls_connect(fd, &ssl, &cfg) < 0) {
 *       // handle error
 *   }
 *
 *   // Use tls_read_full/tls_write_full for I/O
 *   tls_write_full(ssl, data, len);
 *   tls_read_full(ssl, buf, n);
 *
 *   // Disconnect
 *   tls_disconnect(ssl);
 *
 *   // Cleanup at shutdown
 *   tls_cleanup();
 */

#ifndef P9_TLS_H
#define P9_TLS_H

#include <stdint.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

/* ============== Constants ============== */

/* Default port for TLS 9P connections (plaintext uses P9_PORT = 10000) */
#define P9_TLS_PORT 10001

/* ============== Configuration ============== */

/*
 * TLS connection configuration.
 *
 * Exactly one of cert_file, cert_fingerprint, or insecure should be set.
 * If none are set, the connection will fail certificate verification.
 */
struct tls_config {
    /*
     * Path to pinned certificate in PEM format.
     * Server's certificate must exactly match this file.
     */
    char *cert_file;

    /*
     * SHA256 fingerprint of expected certificate as hex string.
     * 64 characters (32 bytes as hex). Colons/spaces are ignored.
     * Example: "a1b2c3d4e5f6..." or "a1:b2:c3:d4:..."
     */
    char *cert_fingerprint;

    /*
     * Skip certificate verification (DANGEROUS).
     * Connection is encrypted but server identity is NOT verified.
     * Vulnerable to man-in-the-middle attacks.
     * Use only for testing or to discover server's fingerprint.
     */
    int insecure;
};

/* ============== Initialization ============== */

/*
 * Initialize OpenSSL library and create global SSL context.
 *
 * Must be called once before any other TLS functions.
 * Creates shared SSL_CTX with TLS 1.2 minimum version.
 *
 * Returns 0 on success, -1 on failure.
 */
int tls_init(void);

/*
 * Clean up OpenSSL resources.
 *
 * Frees global SSL context. Call at program shutdown.
 * After this, tls_init() must be called again to use TLS.
 */
void tls_cleanup(void);

/* ============== Connection Management ============== */

/*
 * Establish TLS connection over existing socket.
 *
 * Performs TLS handshake and verifies server certificate against
 * pinned certificate or fingerprint (unless insecure mode).
 *
 * fd:      connected TCP socket file descriptor
 * ssl_out: output - SSL object on success
 * cfg:     TLS configuration (pinning method)
 *
 * Returns 0 on success, -1 on failure.
 *
 * On success, *ssl_out is set to the SSL object. Use this for
 * subsequent tls_read_full/tls_write_full calls.
 *
 * On failure, the socket is NOT closed (caller's responsibility).
 *
 * In insecure mode, logs the server's fingerprint for future pinning.
 */
int tls_connect(int fd, SSL **ssl_out, struct tls_config *cfg);

/*
 * Close TLS connection and free SSL object.
 *
 * Sends TLS close_notify alert and frees resources.
 * Does NOT close the underlying socket.
 *
 * ssl: SSL object from tls_connect(), or NULL (no-op)
 */
void tls_disconnect(SSL *ssl);

/* ============== Certificate Verification ============== */

/*
 * Verify server certificate against pinned certificate or fingerprint.
 *
 * Called automatically by tls_connect() unless insecure mode.
 * Requires an established SSL connection with a valid peer certificate
 * (i.e., the TLS handshake must have completed).
 *
 * ssl: established SSL connection
 * cfg: configuration with cert_file or cert_fingerprint
 *
 * Returns 0 if certificate matches, -1 on mismatch or error.
 *
 * On mismatch, logs both expected and actual fingerprints.
 */
int tls_verify_pinned(SSL *ssl, struct tls_config *cfg);

/* ============== I/O Operations ============== */

/*
 * Read exactly n bytes through TLS connection.
 *
 * Blocks until all bytes are read or an error occurs.
 * Handles SSL_ERROR_WANT_READ/WANT_WRITE internally.
 *
 * ssl: SSL connection
 * buf: buffer to read into (must have n bytes available)
 * n:   number of bytes to read
 *
 * Returns n on success, -1 on error or connection close.
 */
int tls_read_full(SSL *ssl, uint8_t *buf, int n);

/*
 * Write exactly len bytes through TLS connection.
 *
 * Blocks until all bytes are written or an error occurs.
 * Handles SSL_ERROR_WANT_READ/WANT_WRITE internally.
 *
 * ssl: SSL connection
 * buf: data to write
 * len: number of bytes to write
 *
 * Returns len on success, -1 on error.
 */
int tls_write_full(SSL *ssl, const uint8_t *buf, int len);

/* ============== Utility Functions ============== */

/*
 * Compute SHA256 fingerprint of a PEM certificate file.
 *
 * Useful for obtaining a fingerprint to use for pinning.
 *
 * path:   path to PEM certificate file
 * out:    output buffer for hex string (must be at least 65 bytes)
 * outlen: size of output buffer
 *
 * Returns 0 on success, -1 on error.
 *
 * Output is lowercase hex string, e.g.:
 *   "a1b2c3d4e5f6789012345678901234567890123456789012345678901234"
 */
int tls_cert_file_fingerprint(const char *path, char *out, int outlen);

#endif /* P9_TLS_H */
