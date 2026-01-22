/*
 * p9_tls.c - TLS support for 9P connections
 *
 * Provides TLS transport with certificate pinning for secure
 * connections to Plan 9 systems.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <wlr/util/log.h>

#include "p9_tls.h"

/* Global SSL context - shared by all connections */
static SSL_CTX *g_ctx = NULL;

int tls_init(void) {
    /* OpenSSL 1.1.0+ auto-initializes, but explicit init doesn't hurt */
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                     OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

    g_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ctx) {
        wlr_log(WLR_ERROR, "TLS: Failed to create SSL context");
        ERR_print_errors_fp(stderr);
        return -1;
    }

    /* Require TLS 1.2 minimum, prefer 1.3 */
    SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);

    /* We do our own certificate verification via pinning,
     * so disable the default verification */
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);

    wlr_log(WLR_INFO, "TLS: Initialized (OpenSSL %s)",
            OpenSSL_version(OPENSSL_VERSION));

    return 0;
}

void tls_cleanup(void) {
    if (g_ctx) {
        SSL_CTX_free(g_ctx);
        g_ctx = NULL;
    }
}

/*
 * Load X509 certificate from PEM file.
 *
 * PEM format: Base64 with -----BEGIN CERTIFICATE----- headers
 */
static X509 *load_cert_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        wlr_log(WLR_ERROR, "TLS: Cannot open certificate file '%s': %s",
                path, strerror(errno));
        return NULL;
    }

    X509 *cert = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);

    if (!cert) {
        wlr_log(WLR_ERROR, "TLS: Failed to parse PEM certificate from '%s'", path);
        wlr_log(WLR_ERROR, "TLS: Make sure the certificate is in PEM format");
        wlr_log(WLR_ERROR, "TLS: (should start with -----BEGIN CERTIFICATE-----)");
        wlr_log(WLR_ERROR, "TLS: On 9front, generate with:");
        wlr_log(WLR_ERROR, "TLS:   auth/rsa2x509 ... | auth/pemencode CERTIFICATE > cert");
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    wlr_log(WLR_DEBUG, "TLS: Loaded PEM certificate from '%s'", path);
    return cert;
}

/* Compute SHA256 fingerprint of certificate, write as lowercase hex */
static int cert_fingerprint(X509 *cert, char *out, int outlen) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    unsigned int len = SHA256_DIGEST_LENGTH;

    if (!X509_digest(cert, EVP_sha256(), hash, &len)) {
        wlr_log(WLR_ERROR, "TLS: Failed to compute certificate digest");
        return -1;
    }

    if (outlen < (int)(len * 2 + 1)) {
        wlr_log(WLR_ERROR, "TLS: Fingerprint buffer too small");
        return -1;
    }

    for (unsigned int i = 0; i < len; i++) {
        sprintf(out + i * 2, "%02x", hash[i]);
    }
    out[len * 2] = '\0';

    return 0;
}

int tls_cert_file_fingerprint(const char *path, char *out, int outlen) {
    X509 *cert = load_cert_file(path);
    if (!cert) {
        return -1;
    }

    int rc = cert_fingerprint(cert, out, outlen);
    X509_free(cert);
    return rc;
}

/* Normalize fingerprint string: remove colons/spaces, lowercase */
static void normalize_fingerprint(const char *in, char *out, int outlen) {
    int j = 0;
    for (int i = 0; in[i] && j < outlen - 1; i++) {
        char c = in[i];
        if (c == ':' || c == ' ' || c == '-') {
            continue;  /* Skip separators */
        }
        out[j++] = tolower((unsigned char)c);
    }
    out[j] = '\0';
}

/* Compare two certificates for equality */
static int certs_equal(X509 *a, X509 *b) {
    if (!a || !b) return 0;
    return X509_cmp(a, b) == 0;
}

int tls_verify_pinned(SSL *ssl, struct tls_config *cfg) {
    /* Get server's certificate */
    X509 *server_cert = SSL_get_peer_certificate(ssl);
    if (!server_cert) {
        wlr_log(WLR_ERROR, "TLS: Server provided no certificate");
        return -1;
    }

    /* Compute server cert fingerprint for logging */
    char server_fp[65];
    if (cert_fingerprint(server_cert, server_fp, sizeof(server_fp)) < 0) {
        X509_free(server_cert);
        return -1;
    }

    wlr_log(WLR_INFO, "TLS: Server certificate fingerprint: %s", server_fp);

    int result = -1;

    if (cfg->cert_file) {
        /* Method 1: Compare against pinned certificate file */
        X509 *pinned_cert = load_cert_file(cfg->cert_file);
        if (!pinned_cert) {
            wlr_log(WLR_ERROR, "TLS: Failed to load pinned certificate from '%s'",
                    cfg->cert_file);
            goto out;
        }

        if (certs_equal(server_cert, pinned_cert)) {
            wlr_log(WLR_INFO, "TLS: Server certificate matches pinned certificate");
            result = 0;
        } else {
            char pinned_fp[65];
            cert_fingerprint(pinned_cert, pinned_fp, sizeof(pinned_fp));

            wlr_log(WLR_ERROR, "TLS: Certificate mismatch!");
            wlr_log(WLR_ERROR, "  Server certificate: %s", server_fp);
            wlr_log(WLR_ERROR, "  Pinned certificate: %s", pinned_fp);
            wlr_log(WLR_ERROR, "  Pinned cert file:   %s", cfg->cert_file);
        }

        X509_free(pinned_cert);

    } else if (cfg->cert_fingerprint) {
        /* Method 2: Compare against fingerprint string */
        char expected_fp[65];
        normalize_fingerprint(cfg->cert_fingerprint, expected_fp, sizeof(expected_fp));

        if (strcasecmp(server_fp, expected_fp) == 0) {
            wlr_log(WLR_INFO, "TLS: Server certificate fingerprint matches");
            result = 0;
        } else {
            wlr_log(WLR_ERROR, "TLS: Fingerprint mismatch!");
            wlr_log(WLR_ERROR, "  Server:   %s", server_fp);
            wlr_log(WLR_ERROR, "  Expected: %s", expected_fp);
        }

    } else {
        /* No pinning configured - should not reach here if insecure mode
         * is handled in tls_connect */
        wlr_log(WLR_ERROR, "TLS: No pinned certificate or fingerprint configured");
    }

out:
    X509_free(server_cert);
    return result;
}

int tls_connect(int fd, SSL **ssl_out, struct tls_config *cfg) {
    if (!g_ctx) {
        wlr_log(WLR_ERROR, "TLS: Not initialized (call tls_init first)");
        return -1;
    }

    SSL *ssl = SSL_new(g_ctx);
    if (!ssl) {
        wlr_log(WLR_ERROR, "TLS: Failed to create SSL object");
        ERR_print_errors_fp(stderr);
        return -1;
    }

    /* Associate SSL object with socket file descriptor */
    if (!SSL_set_fd(ssl, fd)) {
        wlr_log(WLR_ERROR, "TLS: Failed to set file descriptor");
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return -1;
    }

    /* Perform TLS handshake */
    wlr_log(WLR_INFO, "TLS: Starting handshake...");

    int ret = SSL_connect(ssl);
    if (ret != 1) {
        int err = SSL_get_error(ssl, ret);
        wlr_log(WLR_ERROR, "TLS: Handshake failed (SSL error %d)", err);
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return -1;
    }

    /* Log connection details */
    wlr_log(WLR_INFO, "TLS: Handshake complete");
    wlr_log(WLR_INFO, "TLS: Protocol: %s, Cipher: %s",
            SSL_get_version(ssl), SSL_get_cipher_name(ssl));

    /* Verify server certificate */
    if (cfg->insecure) {
        wlr_log(WLR_ERROR, "TLS: WARNING - Certificate verification DISABLED");
        wlr_log(WLR_ERROR, "TLS: Connection is encrypted but server identity is NOT verified");
        wlr_log(WLR_ERROR, "TLS: This is vulnerable to man-in-the-middle attacks!");

        /* Still log the fingerprint so user can pin it later */
        X509 *cert = SSL_get_peer_certificate(ssl);
        if (cert) {
            char fp[65];
            if (cert_fingerprint(cert, fp, sizeof(fp)) == 0) {
                wlr_log(WLR_INFO, "TLS: Server fingerprint (for pinning): %s", fp);
            }
            X509_free(cert);
        }
    } else {
        if (tls_verify_pinned(ssl, cfg) < 0) {
            wlr_log(WLR_ERROR, "TLS: Certificate verification failed - aborting");
            SSL_shutdown(ssl);
            SSL_free(ssl);
            return -1;
        }
    }

    *ssl_out = ssl;
    return 0;
}

void tls_disconnect(SSL *ssl) {
    if (ssl) {
        /* Send close_notify alert */
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
}

int tls_write_full(SSL *ssl, const uint8_t *buf, int len) {
    int total = 0;

    while (total < len) {
        int w = SSL_write(ssl, buf + total, len - total);
        if (w <= 0) {
            int err = SSL_get_error(ssl, w);

            /* Handle non-fatal errors */
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                continue;  /* Retry */
            }

            wlr_log(WLR_ERROR, "TLS: Write failed (SSL error %d)", err);
            if (err == SSL_ERROR_SYSCALL) {
                wlr_log(WLR_ERROR, "TLS: System error: %s", strerror(errno));
            }
            ERR_print_errors_fp(stderr);
            return -1;
        }
        total += w;
    }

    return total;
}

int tls_read_full(SSL *ssl, uint8_t *buf, int n) {
    int total = 0;

    while (total < n) {
        int r = SSL_read(ssl, buf + total, n - total);
        if (r <= 0) {
            int err = SSL_get_error(ssl, r);

            /* Handle non-fatal errors */
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue;  /* Retry */
            }

            /* Connection closed cleanly */
            if (err == SSL_ERROR_ZERO_RETURN) {
                wlr_log(WLR_INFO, "TLS: Connection closed by peer");
                return -1;
            }

            wlr_log(WLR_ERROR, "TLS: Read failed (SSL error %d)", err);
            if (err == SSL_ERROR_SYSCALL) {
                if (errno == 0) {
                    wlr_log(WLR_INFO, "TLS: Connection reset by peer");
                } else {
                    wlr_log(WLR_ERROR, "TLS: System error: %s", strerror(errno));
                }
            }
            ERR_print_errors_fp(stderr);
            return -1;
        }
        total += r;
    }

    return total;
}
