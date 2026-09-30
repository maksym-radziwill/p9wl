#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
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
#include <freerdp/log.h>
#include "types.h"

#define TAG FREERDP_TAG("p9wl.tls")

static SSL_CTX *g_ctx = NULL;

int tls_init(void) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                     OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

    g_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ctx) {
        wlr_log(WLR_ERROR, "TLS: Failed to create SSL context");
        WLog_ERR(TAG, "TLS: Failed to create SSL context");
        ERR_print_errors_fp(stderr);
        return -1;
    }

    /* Require TLS 1.2 minimum, prefer 1.3 */
    SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);

    wlr_log(WLR_INFO, "TLS: Initialized (OpenSSL %s)", OpenSSL_version(OPENSSL_VERSION));
    WLog_INFO(TAG, "TLS: Initialized (OpenSSL %s)", OpenSSL_version(OPENSSL_VERSION));

    return 0;
}

void tls_cleanup(void) {
    if (g_ctx) {
        SSL_CTX_free(g_ctx);
        g_ctx = NULL;
    }
}

static X509 *load_cert_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        wlr_log(WLR_ERROR, "TLS: Cannot open certificate file '%s': %s", path, strerror(errno));
        WLog_ERR(TAG, "TLS: Cannot open certificate file '%s': %s", path, strerror(errno));
        return NULL;
    }

    X509 *cert = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);

    if (!cert) {
        wlr_log(WLR_ERROR, "TLS: Failed to parse PEM certificate from '%s'", path);
        WLog_ERR(TAG, "TLS: Failed to parse PEM certificate from '%s'", path);
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    wlr_log(WLR_DEBUG, "TLS: Loaded PEM certificate from '%s'", path);
    WLog_DBG(TAG, "TLS: Loaded PEM certificate from '%s'", path);
    return cert;
}

static int cert_fingerprint(X509 *cert, char *out, int outlen) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    unsigned int len = SHA256_DIGEST_LENGTH;

    if (!X509_digest(cert, EVP_sha256(), hash, &len)) {
        wlr_log(WLR_ERROR, "TLS: Failed to compute certificate digest");
        WLog_ERR(TAG, "TLS: Failed to compute certificate digest");
        return -1;
    }

    if (outlen < (int)(len * 2 + 1)) {
        wlr_log(WLR_ERROR, "TLS: Fingerprint buffer too small");
        WLog_ERR(TAG, "TLS: Fingerprint buffer too small");
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
    if (!cert) return -1;

    int rc = cert_fingerprint(cert, out, outlen);
    X509_free(cert);
    return rc;
}

static void normalize_fingerprint(const char *in, char *out, int outlen) {
    int j = 0;
    for (int i = 0; in[i] && j < outlen - 1; i++) {
        char c = in[i];
        if (c == ':' || c == ' ' || c == '-') continue;
        out[j++] = tolower((unsigned char)c);
    }
    out[j] = '\0';
}

static int certs_equal(X509 *a, X509 *b) {
    if (!a || !b) return 0;
    return X509_cmp(a, b) == 0;
}

int tls_verify_pinned(SSL *ssl, struct tls_config *cfg) {
    X509 *server_cert = SSL_get_peer_certificate(ssl);
    if (!server_cert) {
        wlr_log(WLR_ERROR, "TLS: Server provided no certificate");
        WLog_ERR(TAG, "TLS: Server provided no certificate");
        return -1;
    }

    char server_fp[65];
    if (cert_fingerprint(server_cert, server_fp, sizeof(server_fp)) < 0) {
        X509_free(server_cert);
        return -1;
    }

    wlr_log(WLR_INFO, "TLS: Server certificate fingerprint: %s", server_fp);
    WLog_INFO(TAG, "TLS: Server certificate fingerprint: %s", server_fp);

    int result = -1;

    if (cfg->cert_file) {
        X509 *pinned_cert = load_cert_file(cfg->cert_file);
        if (!pinned_cert) {
            wlr_log(WLR_ERROR, "TLS: Failed to load pinned certificate from '%s'", cfg->cert_file);
            WLog_ERR(TAG, "TLS: Failed to load pinned certificate from '%s'", cfg->cert_file);
            goto out;
        }

        if (certs_equal(server_cert, pinned_cert)) {
            wlr_log(WLR_INFO, "TLS: Server certificate matches pinned certificate");
            WLog_INFO(TAG, "TLS: Server certificate matches pinned certificate");
            result = 0;
        } else {
            char pinned_fp[65];
            cert_fingerprint(pinned_cert, pinned_fp, sizeof(pinned_fp));
            wlr_log(WLR_ERROR, "TLS: Certificate mismatch! Server: %s | Pinned: %s", server_fp, pinned_fp);
            WLog_ERR(TAG, "TLS: Certificate mismatch! Server: %s | Pinned: %s", server_fp, pinned_fp);
        }
        X509_free(pinned_cert);
    } else if (cfg->cert_fingerprint) {
        char expected_fp[65];
        normalize_fingerprint(cfg->cert_fingerprint, expected_fp, sizeof(expected_fp));

        if (strcasecmp(server_fp, expected_fp) == 0) {
            wlr_log(WLR_INFO, "TLS: Server certificate fingerprint matches");
            WLog_INFO(TAG, "TLS: Server certificate fingerprint matches");
            result = 0;
        } else {
            wlr_log(WLR_ERROR, "TLS: Fingerprint mismatch! Server: %s | Expected: %s", server_fp, expected_fp);
            WLog_ERR(TAG, "TLS: Fingerprint mismatch! Server: %s | Expected: %s", server_fp, expected_fp);
        }
    } else {
        wlr_log(WLR_ERROR, "TLS: No pinned certificate or fingerprint configured");
        WLog_ERR(TAG, "TLS: No pinned certificate or fingerprint configured");
    }

out:
    X509_free(server_cert);
    return result;
}

int tls_connect(int fd, SSL **ssl_out, struct tls_config *cfg) {
    if (!g_ctx) {
        wlr_log(WLR_ERROR, "TLS: Not initialized");
        WLog_ERR(TAG, "TLS: Not initialized");
        return -1;
    }

    SSL *ssl = SSL_new(g_ctx);
    if (!ssl) {
        wlr_log(WLR_ERROR, "TLS: Failed to create SSL object");
        WLog_ERR(TAG, "TLS: Failed to create SSL object");
        ERR_print_errors_fp(stderr);
        return -1;
    }

    if (!SSL_set_fd(ssl, fd)) {
        wlr_log(WLR_ERROR, "TLS: Failed to set file descriptor");
        WLog_ERR(TAG, "TLS: Failed to set file descriptor");
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return -1;
    }

    wlr_log(WLR_INFO, "TLS: Starting handshake...");
    WLog_INFO(TAG, "TLS: Starting handshake...");

    int ret = SSL_connect(ssl);
    if (ret != 1) {
        int err = SSL_get_error(ssl, ret);
        wlr_log(WLR_ERROR, "TLS: Handshake failed (SSL error %d)", err);
        WLog_ERR(TAG, "TLS: Handshake failed (SSL error %d)", err);
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return -1;
    }

    wlr_log(WLR_INFO, "TLS: Handshake complete (%s, %s)", SSL_get_version(ssl), SSL_get_cipher_name(ssl));
    WLog_INFO(TAG, "TLS: Handshake complete (%s, %s)", SSL_get_version(ssl), SSL_get_cipher_name(ssl));

    if (cfg->insecure) {
        wlr_log(WLR_ERROR, "TLS: WARNING - Certificate verification DISABLED");
        WLog_ERR(TAG, "TLS: WARNING - Certificate verification DISABLED");
    } else {
        if (tls_verify_pinned(ssl, cfg) < 0) {
            wlr_log(WLR_ERROR, "TLS: Certificate verification failed - aborting");
            WLog_ERR(TAG, "TLS: Certificate verification failed - aborting");
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
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) continue;
            wlr_log(WLR_ERROR, "TLS: Write failed (SSL error %d)", err);
            WLog_ERR(TAG, "TLS: Write failed (SSL error %d)", err);
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
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            if (err == SSL_ERROR_ZERO_RETURN) {
                wlr_log(WLR_INFO, "TLS: Connection closed by peer");
                WLog_INFO(TAG, "TLS: Connection closed by peer");
                return -1;
            }
            wlr_log(WLR_ERROR, "TLS: Read failed (SSL error %d)", err);
            WLog_ERR(TAG, "TLS: Read failed (SSL error %d)", err);
            return -1;
        }
        total += r;
    }
    return total;
}