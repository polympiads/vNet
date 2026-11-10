#!/usr/bin/env python3
"""Generate every vNet file for an event from one roster.

The roster is a text file. Blank lines and lines starting with # are
ignored. Three kinds of lines:

    switch <name>
    agent  <name> <virtual ip> [group,group,...] [internet=cidr,cidr,...] [lockdown]
    allow  <group or name> <group or name>

An agent is in the groups it lists (its own name is always a group of
one). "allow a b" lets every member of a talk to every member of b, in
both directions; "allow x x" connects a group to itself. "internet="
allows those internet addresses or prefixes to the agent; "lockdown"
turns the host firewall on for that machine.

Example:

    switch switch1
    switch switch2
    agent  judge 10.0.0.1 judges internet=0.0.0.0/0
    agent  c001  10.0.1.1 contestants lockdown
    agent  c002  10.0.1.2 contestants lockdown
    allow  contestants judges

Output, in the given directory:

    auth.txt                 the conductor's VNET_AUTH_FILE
    switch.txt               the config every switch loads
    machines/<name>.env      environment for that machine's systemd unit
    certs/                   with --tls: a CA and one certificate per machine

Keys are random and written only to auth.txt and the machine's env file;
rerunning keeps keys for machines already in auth.txt.
"""

import argparse
import ipaddress
import os
import re
import secrets
import subprocess
import sys

NAME_RE = re.compile(r"^[A-Za-z0-9_-]+$")
AGENT_NAME_MAX = 10
SWITCH_NAME_MAX = 32


def fail(line_no, msg):
    sys.exit("roster line %d: %s" % (line_no, msg))


def parse_roster(path):
    switches, agents, allows = [], {}, []
    groups = {}
    with open(path) as f:
        for line_no, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            kind = parts[0]
            if kind == "switch":
                if len(parts) != 2:
                    fail(line_no, "expected: switch <name>")
                name = parts[1]
                if not NAME_RE.match(name) or len(name) > SWITCH_NAME_MAX:
                    fail(line_no, "bad switch name %r" % name)
                switches.append(name)
            elif kind == "agent":
                if len(parts) < 3:
                    fail(line_no, "expected: agent <name> <ip> [groups] [internet=...] [lockdown]")
                name, ip = parts[1], parts[2]
                if not NAME_RE.match(name) or len(name) > AGENT_NAME_MAX:
                    fail(line_no, "agent name %r must be 1 to %d characters from [A-Za-z0-9_-]"
                         % (name, AGENT_NAME_MAX))
                try:
                    ipaddress.IPv4Address(ip)
                except ValueError:
                    fail(line_no, "bad IPv4 address %r" % ip)
                if name in agents:
                    fail(line_no, "agent %s given twice" % name)
                agent = {"ip": ip, "groups": [name], "internet": [], "lockdown": False}
                for extra in parts[3:]:
                    if extra == "lockdown":
                        agent["lockdown"] = True
                    elif extra.startswith("internet="):
                        for cidr in extra[len("internet="):].split(","):
                            if not cidr:
                                continue
                            try:
                                net = ipaddress.IPv4Network(cidr, strict=False)
                            except ValueError:
                                fail(line_no, "bad internet address %r" % cidr)
                            agent["internet"].append(str(net) if "/" in cidr else cidr)
                    else:
                        for g in extra.split(","):
                            if g and not NAME_RE.match(g):
                                fail(line_no, "bad group name %r" % g)
                            if g:
                                agent["groups"].append(g)
                agents[name] = agent
                for g in agent["groups"]:
                    groups.setdefault(g, set()).add(name)
            elif kind == "allow":
                if len(parts) != 3:
                    fail(line_no, "expected: allow <group> <group>")
                allows.append((line_no, parts[1], parts[2]))
            else:
                fail(line_no, "unknown line kind %r" % kind)

    ips = {}
    for name, a in agents.items():
        if a["ip"] in ips:
            sys.exit("agents %s and %s share %s" % (ips[a["ip"]], name, a["ip"]))
        ips[a["ip"]] = name
    if not switches:
        sys.exit("the roster names no switch")
    for line_no, a, b in allows:
        for g in (a, b):
            if g not in groups:
                fail(line_no, "unknown group or agent %r" % g)
    return switches, agents, allows, groups


def existing_keys(auth_path):
    keys = {}
    if not os.path.exists(auth_path):
        return keys
    with open(auth_path) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 3 and parts[0] in ("switch", "agent"):
                keys[(parts[0], parts[1])] = parts[2]
    return keys


def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 epilog="Run with -h for the roster format.")
    ap.add_argument("roster")
    ap.add_argument("outdir")
    ap.add_argument("--conductor", required=True,
                    help="address agents and switches dial, host:port or host (port 5000)")
    ap.add_argument("--switch-port", type=int, default=6000,
                    help="first switch port; switches get consecutive ports (default 6000)")
    ap.add_argument("--tls", action="store_true",
                    help="issue a CA and per-machine certificates into outdir/certs")
    ap.add_argument("--cert-tool", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "vnet-certs.sh"),
                    help="path to vnet-certs.sh")
    ap.add_argument("--install-dir", default="/etc/vnet",
                    help="where the env files will live on the machines (default /etc/vnet)")
    args = ap.parse_args()

    host, _, port = args.conductor.partition(":")
    port = int(port) if port else 5000

    switches, agents, allows, groups = parse_roster(args.roster)
    os.makedirs(os.path.join(args.outdir, "machines"), exist_ok=True)
    auth_path = os.path.join(args.outdir, "auth.txt")
    keys = existing_keys(auth_path)

    def key_for(role, name):
        if (role, name) not in keys:
            keys[(role, name)] = secrets.token_hex(16)
        return keys[(role, name)]

    # auth.txt
    lines = ["# generated by vnet-roster.py; one line per machine"]
    for sw in switches:
        lines.append("switch %s %s" % (sw, key_for("switch", sw)))
    for name in agents:
        lines.append("agent %s %s" % (name, key_for("agent", name)))
    write_if_changed(auth_path, "\n".join(lines) + "\n")

    # switch.txt
    lines = ["# generated by vnet-roster.py; copy to every switch", "", "# virtual addresses"]
    for name, a in agents.items():
        lines.append("%s %s" % (name, a["ip"]))
    pairs = set()
    for _, a, b in allows:
        for x in groups[a]:
            for y in groups[b]:
                if x != y:
                    pairs.add(tuple(sorted((x, y))))
    lines += ["", "# who may talk to whom (each rule works both ways)"]
    lines += ["%s %s" % p for p in sorted(pairs)]
    lines += ["", "# internet access"]
    for name, a in agents.items():
        for cidr in a["internet"]:
            lines.append("%s ::internet:%s" % (name, cidr))
    write_if_changed(os.path.join(args.outdir, "switch.txt"), "\n".join(lines) + "\n")

    # certificates
    tls_lines = []
    if args.tls:
        certs = os.path.join(args.outdir, "certs")
        if not os.path.exists(os.path.join(certs, "ca.pem")):
            subprocess.run([args.cert_tool, "init", certs], check=True)
        for role, name in [("conductor", "conductor")] + [("switch", s) for s in switches] + \
                          [("agent", n) for n in agents]:
            if not os.path.exists(os.path.join(certs, name + ".pem")):
                subprocess.run([args.cert_tool, "issue", certs, role, name], check=True)

        def tls_env(name):
            d = args.install_dir
            return ["VNET_TLS=1", "VNET_TLS_CA=%s/certs/ca.pem" % d,
                    "VNET_TLS_CERT=%s/certs/%s.pem" % (d, name),
                    "VNET_TLS_KEY=%s/certs/%s.key" % (d, name)]
    else:
        def tls_env(name):
            return ["VNET_ALLOW_INSECURE=1"]

    # machine env files
    common = ["CONDUCTOR_IP=%s" % host, "CONDUCTOR_PORT=%d" % port]
    write_if_changed(os.path.join(args.outdir, "machines", "conductor.env"), "\n".join(
        ["# conductor", "CONDUCTOR_PORT=%d" % port,
         "VNET_AUTH_FILE=%s/auth.txt" % args.install_dir] + tls_env("conductor")) + "\n")
    for i, sw in enumerate(switches):
        write_if_changed(os.path.join(args.outdir, "machines", sw + ".env"), "\n".join(
            ["# switch %s" % sw, "VNET_SWITCH_NAME=%s" % sw,
             "VNET_SWITCH_PORT=%d" % (args.switch_port + i),
             "VNET_AUTH_KEY=%s" % key_for("switch", sw),
             "VNET_CONFIG_PATH=%s/switch.txt" % args.install_dir] + common + tls_env(sw)) + "\n")
    for name, a in agents.items():
        env = ["# agent %s (%s)" % (name, a["ip"]), "VNET_AGENT_NAME=%s" % name,
               "VNET_AUTH_KEY=%s" % key_for("agent", name)] + common + tls_env(name)
        if a["lockdown"]:
            env.append("VNET_LOCKDOWN=1")
        write_if_changed(os.path.join(args.outdir, "machines", name + ".env"), "\n".join(env) + "\n")

    print("wrote %s: %d switch(es), %d agent(s), %d allowed pair(s)%s"
          % (args.outdir, len(switches), len(agents), len(pairs), ", certificates" if args.tls else ""))


if __name__ == "__main__":
    main()
