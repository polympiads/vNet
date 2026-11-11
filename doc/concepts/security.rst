Security model
==============

vNet is built for an event where the contestant machines are not trusted:
a contestant may be root on the machine, may stop or replace the agent,
and may send any packet. The organizer machines (conductor and switches)
are trusted. This page says what vNet guarantees under that assumption and
what it leaves to you.

What a contestant machine cannot do
-----------------------------------

Reach a machine the rules do not allow
   Every packet is checked by a switch against the access list, and checked
   again by the receiving switch when it crosses between switches. Nothing
   the machine does changes which rules apply to it.

Pretend to be another machine
   A switch binds each connection to the virtual address the conductor
   assigned to that name, and drops every packet with another source
   address. The name comes from the conductor, which checked the machine's
   key, never from the machine (:doc:`joining`).

Join without a key and a certificate
   The key is checked against the auth file, the certificate against the
   CA and its role. The certificate's name is not compared with the
   machine name: what ties a machine to its name is the key. Keep each
   machine's key on that machine only.

Pose as a switch or as the conductor
   With ``VNET_TLS=1`` every link is mutual TLS, and each program checks
   the role written in the peer certificate's OU field: an agent
   certificate cannot open a switch or conductor link. A switch also
   refuses a hello from a peer the conductor did not pair it with, with a
   per-pair token.

Flood a switch
   The per-agent rate limit caps what one machine can send; the switch
   serves connections in turns, so one busy link does not hold the others.

Read other machines' traffic on the wire
   The links are TLS; packets between machines travel inside them.

What vNet does not do
---------------------

Filter the physical network
   The switch only sees what enters the TUN. A machine can still reach
   its physical LAN, and the internet over IPv6, unless the host firewall
   blocks it. That is what lockdown is for (:doc:`../guides/vdi-lockdown`).
   Lockdown is enforced by the agent on a machine you do not trust: it
   stops honest mistakes and casual attempts, not a contestant who is root
   and removes it. For a hard guarantee, filter at the physical network as
   well (switch ports, VLANs, the room's firewall), so that a machine's
   only way out is the overlay.

Filter ports
   An allowed pair of machines may use any protocol and port.

Inspect content
   vNet decides by addresses only.

Protect a compromised organizer machine
   A switch or conductor with a stolen key and certificate is trusted.
   Keep ``ca.key`` off every machine: it is needed only to issue
   certificates.

Keys and certificates
---------------------

* Each machine has a key (a random string in the auth file and in the
  machine's environment) and, with TLS, its own certificate and private
  key. ``vnet-roster`` generates both
  (:doc:`../reference/roster`); the certificates come from
  ``tools/vnet-certs.sh`` (:doc:`../guides/certificates`).
* Give the key in ``VNET_AUTH_KEY_FILE`` or ``VNET_AUTH_KEY`` rather than
  on the command line, where every local user can read it in the process
  list.
* Keys are compared in constant time.
* The conductor refuses to start without an auth file and TLS, and the
  switch and agent refuse to start without TLS, unless
  ``VNET_ALLOW_INSECURE=1`` is set. Use that only in a lab.
