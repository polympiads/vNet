#include "vnet/tls/session.hpp"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace vnet::tls {

    static SSL_CTX* g_ctx = nullptr;
    static bool g_enabled = false;
    static bool g_role_check = true;

    bool enabled() { return g_enabled; }

    static void log_error(const char* what) {
        std::cerr << "[TLS] " << what << "\n";
        ERR_print_errors_fp(stderr);
    }

    bool init() {
        const char* flag = std::getenv("VNET_TLS");
        if (!flag || std::string(flag) != "1")
            return true;

        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();

        g_ctx = SSL_CTX_new(TLS_method());
        if (!g_ctx) {
            log_error("SSL_CTX_new");
            return false;
        }
        SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);
        SSL_CTX_set_mode(g_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                                SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

        const char* cert = std::getenv("VNET_TLS_CERT");
        const char* key  = std::getenv("VNET_TLS_KEY");
        const char* ca   = std::getenv("VNET_TLS_CA");
        if (!cert || !key || !ca) {
            std::cerr << "[TLS] VNET_TLS_CERT, VNET_TLS_KEY and VNET_TLS_CA are required\n";
            return false;
        }
        if (SSL_CTX_use_certificate_file(g_ctx, cert, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(g_ctx, key, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(g_ctx) != 1 ||
            SSL_CTX_load_verify_locations(g_ctx, ca, nullptr) != 1) {
            log_error("certificate load");
            return false;
        }
        SSL_CTX_set_verify(g_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        g_enabled = true;

        const char* role_check = std::getenv("VNET_TLS_ROLE_CHECK");
        g_role_check = !(role_check && std::string(role_check) == "0");
        std::cout << "[TLS] Enabled, peer certificates required"
                  << (g_role_check ? ", role in OU checked" : ", role check off") << "\n";
        return true;
    }

    static ssl_st* handshake(int fd, bool server) {
        SSL* ssl = SSL_new(g_ctx);
        if (!ssl) return nullptr;
        SSL_set_fd(ssl, fd);
        int rc = server ? SSL_accept(ssl) : SSL_connect(ssl);
        if (rc != 1) {
            int serr = SSL_get_error(ssl, rc);
            unsigned long err = ERR_peek_error();
            // A short socket timeout, or a health check that hangs up, is a
            // retry. It is not a certificate failure.
            bool timed_out = serr == SSL_ERROR_WANT_READ ||
                             serr == SSL_ERROR_WANT_WRITE ||
                             (serr == SSL_ERROR_SYSCALL &&
                              (errno == EAGAIN || errno == EWOULDBLOCK ||
                               errno == ETIMEDOUT));
            if (!timed_out &&
                ERR_GET_REASON(err) != SSL_R_UNEXPECTED_EOF_WHILE_READING)
                log_error(server ? "accept" : "connect");
            else
                ERR_clear_error();
            SSL_free(ssl);
            return nullptr;
        }
        return ssl;
    }

    ssl_st* new_session(int fd) {
        if (!g_enabled) return nullptr;
        SSL* ssl = SSL_new(g_ctx);
        if (!ssl) return nullptr;
        if (SSL_set_fd(ssl, fd) != 1) {
            SSL_free(ssl);
            return nullptr;
        }
        return ssl;
    }

    ssl_st* handshake_client(int fd) { return handshake(fd, false); }
    ssl_st* handshake_server(int fd) { return handshake(fd, true); }

    std::string peer_role(ssl_st* ssl_in) {
        if (!ssl_in) return "";
        SSL* ssl = reinterpret_cast<SSL*>(ssl_in);
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        X509* cert = SSL_get1_peer_certificate(ssl);
#else
        X509* cert = SSL_get_peer_certificate(ssl);
#endif
        if (!cert) return "";
        char buf[64] = {0};
        int n = X509_NAME_get_text_by_NID(X509_get_subject_name(cert),
                                          NID_organizationalUnitName, buf, sizeof(buf));
        X509_free(cert);
        if (n <= 0) return "";
        return std::string(buf, static_cast<size_t>(n));
    }

    bool peer_has_role(ssl_st* ssl, const char* role) {
        if (!g_enabled || !g_role_check) return true;
        if (!ssl) return false;
        return peer_role(ssl) == role;
    }

    void free_session(ssl_st* ssl) {
        if (!ssl) return;
        SSL_shutdown(reinterpret_cast<SSL*>(ssl));
        SSL_free(reinterpret_cast<SSL*>(ssl));
    }

}
