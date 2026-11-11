Lock down contestant machines
=============================

The switches only see what a machine sends into its TUN. Without more, a
contestant machine still reaches its physical LAN directly, and the
internet over IPv6. *Lockdown* closes that: the agent installs a host
firewall so that the machine talks only through the overlay, plus the
exceptions you list. This page shows how to set it up, in particular for
contestant machines that are VDI desktops.

Turn it on
----------

In the roster, add ``lockdown`` to the machine (and ``droplog`` to log
what it drops):

.. code-block:: text

   agent c001 10.0.1.1 contestants lockdown

or set ``VNET_LOCKDOWN=1`` (and ``VNET_LOCKDOWN_LOG=1``) in the machine's
``/etc/vnet/agent.env``. When the agent joins it logs:

.. code-block:: text

   [Agent] Lockdown on: only the conductor, the switch, the overlay and the bypass file (routes: 1, rules: 1) are reachable; IPv6 is blocked

What lockdown allows
--------------------

The agent puts two chains, ``VNET_IN`` and ``VNET_OUT``, first in the
``INPUT`` and ``OUTPUT`` chains, for both iptables and ip6tables. In
IPv4 they accept, in this order:

* loopback;
* everything on the TUN, ``vnet-<name>`` (the switch decides what goes
  through it);
* outbound TCP to the conductor's port and to the switch's port;
* packets of connections already allowed (``ESTABLISHED,RELATED``);
* DHCP, so the machine keeps its address;
* the ``route`` prefixes of the bypass file, both directions, any port;
* the ``in`` and ``out`` lines of the bypass file;

and drop the rest. In IPv6 they accept loopback and the ``in6`` and
``out6`` lines, and drop the rest.

The chains are rebuilt at every join (the switch may change) and at every
``systemctl reload vnet-agent``. Stopping the agent removes them.

The bypass file
---------------

The bypass file lists what stays on the physical network
(:doc:`../reference/bypass-file`). In the roster, a ``bypass`` line puts
one line in the bypass file of every member of a group:

.. code-block:: text

   bypass contestants route 192.0.2.10/32
   bypass contestants in -p tcp -s 198.51.100.0/24 --dport 22443 -j ACCEPT

``route`` keeps a prefix off the TUN and opens it in the firewall, in both
directions. ``in`` and ``out`` add an iptables rule exactly as written,
for a narrower opening (one port, one direction).

VDI desktops
------------

When the contestant machine is a virtual desktop, the contestant's screen
reaches it over the physical network, from a VDI broker or gateway. That
connection must survive both the default route moving into the TUN and the
firewall. Two ways, depending on who opens the connection:

The desktop connects out to the broker
   Keep the broker on the physical network with a route line:

   .. code-block:: text

      bypass contestants route 192.0.2.10/32

The gateway connects in to the desktop
   Open the port it uses, from its addresses only:

   .. code-block:: text

      bypass contestants in -p tcp -s 198.51.100.0/24 --dport 3389 -j ACCEPT

   The replies leave through the ``ESTABLISHED`` rule. If the gateway is
   not on the desktop's subnet, add a ``route`` line for it as well, so the
   replies go back through the physical gateway rather than into the TUN.

Before the event, check from a locked-down desktop that the session
survives an agent restart: the chains are rebuilt during a join, and the
session must not depend on anything else.

Other things to keep in mind
----------------------------

DNS
   The machine's resolver is usually on the physical network, and lockdown
   blocks it. Either add a ``route`` line for the DNS server, or give the
   machine a resolver reachable through the overlay (a judge machine, or an
   internet resolver allowed with ``internet=``).

Organizer access
   To keep SSH from the organizers' network, add
   ``bypass contestants in -p tcp -s <admin network> --dport 22 -j ACCEPT``.

Time
   NTP to a server on the physical network needs a ``route`` line too.

Other firewall tools
   Lockdown inserts its chains first in ``INPUT`` and ``OUTPUT``. A tool
   that rewrites the whole firewall (``ufw``, ``firewalld``, a
   configuration-management run) may remove them; ``systemctl reload
   vnet-agent`` puts them back.

What lockdown cannot do
   It runs on a machine the contestant may control. A contestant who is
   root can remove it. Filter at the physical network as well when that
   matters (:doc:`../concepts/security`).

See what is dropped
-------------------

With ``droplog`` (``VNET_LOCKDOWN_LOG=1``), every drop is written to the
kernel log, at most one per second with a burst of 50, with one of these
prefixes:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Prefix
     - Dropped
   * - ``vnet-drop-in:``
     - an IPv4 packet arriving
   * - ``vnet-drop-out:``
     - an IPv4 packet leaving
   * - ``vnet-drop-in6:``
     - an IPv6 packet arriving
   * - ``vnet-drop-out6:``
     - an IPv6 packet leaving

.. code-block:: console

   $ sudo journalctl -k -g vnet-drop -n 20

Each line has the addresses and ports, which is what to put in a bypass
line if the traffic should be allowed.

Free a machine by hand
----------------------

``sudo systemctl stop vnet-agent`` removes everything. If the agent is
gone (crashed, uninstalled) and the machine is still closed:

.. code-block:: console

   $ sudo iptables  -D INPUT -j VNET_IN;  sudo iptables  -D OUTPUT -j VNET_OUT
   $ sudo ip6tables -D INPUT -j VNET_IN;  sudo ip6tables -D OUTPUT -j VNET_OUT
   $ sudo ip route replace default via <gateway> dev <interface>

The gateway and interface the machine had before are in
``/run/vnet/agent-<name>.route``.
