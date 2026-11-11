Bypass file
===========

The bypass file of an agent lists what stays on the machine's physical
network while it is joined: addresses that keep their route through the
physical gateway, and, with lockdown, firewall exceptions. The agent reads
it from ``VNET_BYPASS_FILE`` at start and again on ``SIGHUP``
(``systemctl reload vnet-agent``). ``vnet-roster`` writes one per machine
that has ``bypass`` lines, as ``machines/<name>.bypass``, to install as
``/etc/vnet/bypass.conf``.

Format
------

Blank lines and lines whose first non-blank character is ``#`` are
ignored. Five kinds of lines:

``route <a.b.c.d/n>``
   The prefix keeps going through the physical gateway instead of the TUN.
   With lockdown it is also allowed in both directions, any protocol and
   port. A bare address is a ``/32``. The prefix must not be ``0.0.0.0/0``
   and must not overlap the overlay prefix.

``out <iptables arguments>`` and ``in <iptables arguments>``
   A rule appended to the lockdown chain ``VNET_OUT`` or ``VNET_IN``, before
   the final drop, exactly as written. The words are passed to
   ``iptables`` without a shell, so quotes and variables are not
   interpreted. Without lockdown these lines do nothing, and the agent says
   so at start.

``out6 <ip6tables arguments>`` and ``in6 <ip6tables arguments>``
   The same for IPv6, which lockdown otherwise blocks entirely.

Example
-------

.. code-block:: text

   # VDI broker and the organizers' DNS stay on the LAN
   route 192.0.2.10
   route 192.0.2.53/32
   # the VDI gateway connects in
   in  -p tcp -s 198.51.100.0/24 --dport 3389 -j ACCEPT
   # organizers' SSH, over IPv6 too
   in  -p tcp -s 198.51.100.0/24 --dport 22 -j ACCEPT
   in6 -p tcp -s 2001:db8:100::/48 --dport 22 -j ACCEPT

How a route line is applied
---------------------------

Once the default route points into the TUN, the agent pins each route
prefix:

* a prefix inside a subnet the machine is directly on needs nothing, since
  the subnet route already wins;
* a prefix the machine already routes through another gateway is left to
  that route;
* anything else gets a route through the physical gateway that was the
  default before the agent joined.

The agent logs one of ``<prefix> stays on the physical network via <gw>
dev <if>`` or ``<prefix> already has a route via ...`` for each. When a
reload removes a line, its route is removed (``Route <prefix> no longer
bypasses the TUN``).

Errors
------

A file with an error is refused as a whole: at start the agent stops, on a
reload the previous rules stay in force (``Bypass file reload failed``).

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Message
     - Cause
   * - ``Bypass file line N: expected 'route a.b.c.d/n'``
     - a route line without a prefix, with more than one, or malformed
   * - ``Bypass file line N: a route for 0.0.0.0/0 would undo the default route through the TUN``
     - a ``/0`` route
   * - ``Bypass file line N: <prefix> overlaps the overlay <overlay>``
     - a route inside, or containing, the overlay prefix
   * - ``Bypass file line N: '<kind>' needs iptables arguments``
     - an ``in``/``out`` line with nothing after it
   * - ``Bypass file line N: unknown line kind '...'``
     - a first word other than the five above
   * - ``Cannot open bypass file <path>``
     - wrong path or permissions

An ``in``/``out`` line that parses but that iptables rejects is reported
when the chains are built: the iptables error, then ``Bypass rule not
installed: in ...``. The other rules are installed.
