#!/usr/bin/env python3
"""Raw protocol client for the integration tests.

Speaks the 6-byte header (uint32 payload size, uint16 type, big endian)
and hand-encodes the few protobuf messages the checks need, so a test can
send what a real program never would. Run inside the test namespace:

    client.py <check> [args]

Each check prints one line starting with RESULT and exits 0 when the
behavior is the expected one, 1 otherwise.
"""
import socket
import struct
import sys
import time

TYPES = dict(HEARTBEAT=0, SWITCH_MIP=1, AGENT_MIP=2, CONNECT_TO_SWITCH=3,
             AGENT_CONNECTION_TOKEN=4, AUTH_CONNECT_TO_SWITCH=5,
             CONNECTION_ACCEPTED=6, AGENT_MRP=7, IPV4_RAW=10,
             SWITCH_ROUTE_UPDATE=12, SWITCH_DISCONNECTED=13, SWITCH_HELLO=14,
             AGENT_UNREGISTERED=15, PEER_TOKEN=16)

CONDUCTOR = ("127.0.0.1", 5000)
SWITCH = ("127.0.0.1", 6000)


def varint(n):
    out = b""
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out += bytes([b | 0x80])
        else:
            return out + bytes([b])


def f_str(num, s):
    s = s.encode()
    return bytes([(num << 3) | 2]) + varint(len(s)) + s


def f_fixed64(num, v):
    return bytes([(num << 3) | 1]) + struct.pack("<Q", v)


def f_varint(num, v):
    return bytes([(num << 3) | 0]) + varint(v)


def packet(ptype, body=b""):
    return struct.pack("!IH", len(body), TYPES[ptype]) + body


def recv_packet(s, timeout=3.0):
    s.settimeout(timeout)
    try:
        hdr = b""
        while len(hdr) < 6:
            chunk = s.recv(6 - len(hdr))
            if not chunk:
                return None
            hdr += chunk
        size, ptype = struct.unpack("!IH", hdr)
        body = b""
        while len(body) < size:
            chunk = s.recv(size - len(body))
            if not chunk:
                return None
            body += chunk
        return ptype, body
    except (socket.timeout, ConnectionResetError):
        return None


def recv_non_heartbeat(s, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        p = recv_packet(s, max(0.1, end - time.time()))
        if p is None or p[0] != TYPES["HEARTBEAT"]:
            return p
    return None


def closed_by_peer(s, timeout=3.0):
    s.settimeout(timeout)
    try:
        end = time.time() + timeout
        while time.time() < end:
            if not s.recv(4096):
                return True
        return False
    except socket.timeout:
        return False
    except (ConnectionResetError, BrokenPipeError):
        return True


def result(name, ok, detail):
    print("RESULT %s %s %s" % (name, "ok" if ok else "FAIL", detail))
    sys.exit(0 if ok else 1)


def check_oversized():
    """A header announcing a 1 GB payload must close the connection."""
    for name, addr in (("conductor", CONDUCTOR), ("switch", SWITCH)):
        s = socket.create_connection(addr, timeout=3)
        s.sendall(struct.pack("!IH", 1 << 30, TYPES["AGENT_MIP"]))
        if not closed_by_peer(s, 3):
            result("oversized", False, "%s kept the connection open" % name)
    result("oversized", True, "both closed the connection")


def check_forged_token():
    """A client on the agent port forges a token and logs in with it."""
    s = socket.create_connection(SWITCH, timeout=3)
    s.sendall(packet("AGENT_CONNECTION_TOKEN", f_str(1, "agent2") + f_fixed64(2, 42)))
    time.sleep(0.3)
    try:
        s.sendall(packet("AUTH_CONNECT_TO_SWITCH", f_fixed64(1, 42)))
    except (BrokenPipeError, ConnectionResetError):
        result("forged_token", True, "closed after the forged token")
    p = recv_non_heartbeat(s)
    if p and p[0] == TYPES["CONNECTION_ACCEPTED"]:
        result("forged_token", False, "logged in as agent2")
    result("forged_token", True, "rejected")


def check_forged_hello():
    """A client on the agent port claims to be a peer switch."""
    s = socket.create_connection(SWITCH, timeout=3)
    s.sendall(packet("SWITCH_HELLO", f_str(1, "switch9") + f_fixed64(2, 7)))
    result("forged_hello", closed_by_peer(s, 2), "")


def check_forged_withdrawal():
    """A client on the agent port tells the switch a peer went away."""
    s = socket.create_connection(SWITCH, timeout=3)
    s.sendall(packet("SWITCH_DISCONNECTED", f_str(1, "switch2")))
    result("forged_withdrawal", closed_by_peer(s, 2), "")


def check_reidentify():
    """A registered switch connection sends a second MIP."""
    c = socket.create_connection(CONDUCTOR, timeout=3)
    c.sendall(packet("SWITCH_MIP", f_str(1, "switch9") + f_str(2, "k9")
                     + f_str(3, "::internet") + f_varint(4, 6009)))
    time.sleep(0.3)
    c.sendall(packet("SWITCH_MIP", f_str(1, "switch8") + f_str(2, "k8")
                     + f_str(3, "::internet") + f_varint(4, 6008)))
    result("reidentify", closed_by_peer(c, 2), "")


def check_idle(seconds):
    """A connection that never identifies itself is closed."""
    start = time.time()
    socks = [socket.create_connection(a, timeout=3) for a in (CONDUCTOR, SWITCH)]
    for s, name in zip(socks, ("conductor", "switch")):
        remaining = seconds - (time.time() - start)
        if not closed_by_peer(s, remaining):
            result("idle", False, "%s kept an idle connection for %ds" % (name, seconds))
    result("idle", True, "both closed within %ds" % seconds)


def check_bad_key():
    """An agent with the wrong key gets no switch."""
    c = socket.create_connection(CONDUCTOR, timeout=3)
    c.sendall(packet("AGENT_MIP", f_str(1, "agent1") + f_str(2, "wrong") + f_str(3, "vnet.internal")))
    p = recv_non_heartbeat(c)
    result("bad_key", p is None or p[0] != TYPES["CONNECT_TO_SWITCH"], "")


def check_unknown_name():
    """An agent not in the auth file gets no switch."""
    c = socket.create_connection(CONDUCTOR, timeout=3)
    c.sendall(packet("AGENT_MIP", f_str(1, "nobody") + f_str(2, "k1") + f_str(3, "vnet.internal")))
    p = recv_non_heartbeat(c)
    result("unknown_name", p is None or p[0] != TYPES["CONNECT_TO_SWITCH"], "")


if __name__ == "__main__":
    check = sys.argv[1] if len(sys.argv) > 1 else ""
    if check == "oversized":
        check_oversized()
    elif check == "forged_token":
        check_forged_token()
    elif check == "forged_hello":
        check_forged_hello()
    elif check == "forged_withdrawal":
        check_forged_withdrawal()
    elif check == "reidentify":
        check_reidentify()
    elif check == "idle":
        check_idle(int(sys.argv[2]) if len(sys.argv) > 2 else 14)
    elif check == "bad_key":
        check_bad_key()
    elif check == "unknown_name":
        check_unknown_name()
    else:
        sys.exit("unknown check %r" % check)
