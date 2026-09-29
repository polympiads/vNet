#pragma once

struct ssl_st;

namespace vnet::tls {

    /** True after init() when VNET_TLS=1 and the certificate loaded. */
    bool enabled();

    /**
     * Read VNET_TLS, VNET_TLS_CERT, VNET_TLS_KEY and VNET_TLS_CA.
     * When TLS is off this returns true and enabled() stays false.
     */
    bool init();

    /** Blocking handshake. The fd should be blocking. Returns null on failure. */
    ssl_st* handshake_client(int fd);
    ssl_st* handshake_server(int fd);

    void free_session(ssl_st* ssl);

}
