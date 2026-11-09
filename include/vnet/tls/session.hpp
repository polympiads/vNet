#pragma once

#include <string>

struct ssl_st;

namespace vnet::tls {

    /** Certificate roles, carried in the subject's OU field. */
    const char* const ROLE_CONDUCTOR = "conductor";
    const char* const ROLE_SWITCH    = "switch";
    const char* const ROLE_AGENT     = "agent";

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

    /**
     * A session bound to fd with no handshake done yet, for the
     * non-blocking handshake that NetQueue drives. Null when TLS is off or
     * the session cannot be made.
     */
    ssl_st* new_session(int fd);

    void free_session(ssl_st* ssl);

    /**
     * The role named in the peer certificate's OU, or "" when there is no
     * certificate or no OU.
     */
    std::string peer_role(ssl_st* ssl);

    /**
     * True when the peer may act as role: TLS is off, VNET_TLS_ROLE_CHECK=0,
     * or the peer certificate's OU is exactly role. With one certificate
     * per role, a key found on a contestant machine cannot be used to pose
     * as a switch or the conductor.
     */
    bool peer_has_role(ssl_st* ssl, const char* role);

}
