#include "vnet/tls/session.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace vnet::tls {

    static SSL_CTX* g_ctx = nullptr;
    static bool g_enabled = false;

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
        std::cout << "[TLS] Enabled, peer certificates required\n";
        return true;
    }

    static ssl_st* handshake(int fd, bool server) {
        SSL* ssl = SSL_new(g_ctx);
        if (!ssl) return nullptr;
        SSL_set_fd(ssl, fd);
        int rc = server ? SSL_accept(ssl) : SSL_connect(ssl);
        if (rc != 1) {
            unsigned long err = ERR_peek_error();
            // Docker health checks open the port and hang up. That is not a
            // certificate failure.
            if (ERR_GET_REASON(err) != SSL_R_UNEXPECTED_EOF_WHILE_READING)
                log_error(server ? "accept" : "connect");
            else
                ERR_clear_error();
            SSL_free(ssl);
            return nullptr;
        }
        return ssl;
    }

    ssl_st* handshake_client(int fd) { return handshake(fd, false); }
    ssl_st* handshake_server(int fd) { return handshake(fd, true); }

    void free_session(ssl_st* ssl) {
        if (!ssl) return;
        SSL_shutdown(reinterpret_cast<SSL*>(ssl));
        SSL_free(reinterpret_cast<SSL*>(ssl));
    }

}
