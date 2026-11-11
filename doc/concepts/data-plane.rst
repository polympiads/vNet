The data plane
==============

This page follows one IPv4 packet from the machine that sends it to the
machine that receives it, and lists every place where it can be dropped.

Leaving the machine
-------------------

An application on the machine sends a packet. The kernel routes it into
``vnet-<name>``: the overlay prefix always goes there, and with
``VNET_DEFAULT_VIA_TUN=1`` (the default) so does everything that has no
more specific route. The agent reads the packet from the TUN and sends it,
unchanged, to its switch as one ``IPV4_RAW`` message.

The agent carries IPv4 only. With lockdown on, IPv6 is blocked on the host
(:doc:`../guides/vdi-lockdown`).

The switch decides
------------------

The switch checks a packet from one of its agents in this order. The first
check that fails drops the packet and counts it under the reason shown,
which appears in the switch's statistics line
(:doc:`../guides/monitoring`).

.. list-table::
   :header-rows: 1
   :widths: 8 52 20

   * - Step
     - Check
     - Drop reason
   * - 1
     - The bytes are an IPv4 packet with a valid header.
     - ``malformed``
   * - 2
     - The connection belongs to an agent that completed its join.
     - ``unknown-source``
   * - 3
     - The source address is the agent's own virtual address.
     - ``spoof``
   * - 4
     - The agent is within its packet and byte rate
       (``VNET_AGENT_PPS``, ``VNET_AGENT_MBPS``).
     - ``rate-limited``
   * - 5
     - If the destination is a virtual address in the access list: a rule
       connects the two machines. Otherwise the destination is on the
       internet: a ``::internet:`` rule of the sender covers it.
     - ``acl``
   * - 6
     - The switch can reach the destination: it has the agent, a route to
       its switch, or an uplink for the internet.
     - ``unreachable``

A destination that is in the access list but whose machine has not joined
yet, or whose route has not reached this switch yet, is not dropped at
once: the switch keeps up to 16 packets per destination for up to 5
seconds, and sends them when the route arrives. They are counted as
``waited-for-route``.

Any address that is not a virtual address in the access list counts as an
internet address, including an unassigned address inside the overlay
prefix. Such a packet is dropped as ``acl`` unless a ``::internet:`` rule
covers it.

Where the packet goes
---------------------

Local
   The destination is attached to the same switch. The switch writes the
   packet to that agent's connection, and the agent writes it into its TUN.

Another switch
   The destination is attached to another switch. The switch sends the
   packet over its direct link to that switch. The receiving switch checks
   again that the destination is one of its own agents, that the source is
   a configured machine reached through the switch that sent it, and that
   the access list allows the pair; then it delivers. A switch never
   forwards a packet it received from another switch, so a packet crosses
   at most one switch-to-switch link.

Internet
   The switch writes the packet into its ``vnet-out`` TUN. The kernel
   routes it out through the switch's own default route with address
   translation (an iptables ``MASQUERADE`` rule on the uplink interface,
   which the switch adds at start), so the internet sees the switch's
   address. A reply comes back through ``vnet-out``, and the
   switch delivers it only if the receiving machine has a ``::internet:``
   rule for the address it came from.

Rules are symmetric
-------------------

A rule between two machines allows both directions: ``c001 judge`` lets
``c001`` reach ``judge`` and ``judge`` reach ``c001``. A ``::internet:``
rule allows the machine to reach that address, and the replies to come
back; it does not let the internet open connections to the machine, since
the internet only sees the switch's translated address.

There is no port filtering in the switch. If a machine may talk to
another, it may use any protocol and port. Narrow this on the machines
themselves if you need to.

Backpressure
------------

Every link has a write buffer of 2 MiB. When the link to a destination is
full, the switch drops the packet rather than wait, and counts it in
``link-full-drops``. A switch handles at most 64 packets from one
connection before it serves the next, so one busy machine cannot hold the
loop.

The per-agent limit (20000 packets per second and 200 Mbit/s by default,
each with a burst of one second) keeps one machine from taking a switch's
capacity from the others. Set either to 0 to remove it.
