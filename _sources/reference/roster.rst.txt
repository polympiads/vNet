Roster and vnet-roster
======================

``vnet-roster`` (``tools/vnet-roster.py``) generates every file of an event
from one roster: the auth file, the switch config, each machine's
environment, the bypass files and, with ``--tls``, the certificates.

Command
-------

.. code-block:: text

   vnet-roster <roster> <outdir> --conductor <host[:port]>
               [--switch-port 6000] [--tls] [--cert-tool <path>]
               [--install-dir /etc/vnet]

``--conductor``
   The address switches and agents dial; the port defaults to 5000.

``--switch-port``
   The first switch's port; the switches get consecutive ports in roster
   order.

``--tls``
   Make a CA and one certificate per machine in ``<outdir>/certs``, using
   ``vnet-certs.sh`` (or ``--cert-tool``). Without it, every env file gets
   ``VNET_ALLOW_INSECURE=1``.

``--install-dir``
   Where the files will be on the machines; the env files refer to it.

Running it again on the same directory keeps the keys already in
``auth.txt`` and the certificates already issued, so only new machines get
new ones; files whose content did not change are not rewritten. The
keys of the certificates, ``auth.txt``, ``switch.txt``, the env files and
the bypass files are written readable by their owner only.

Roster format
-------------

Blank lines and lines starting with ``#`` are ignored.

``switch <name>``
   A switch. At least one is required.

``agent <name> <address> [groups] [internet=<list>] [lockdown] [droplog]``
   A machine and its virtual address. ``groups`` is a comma-separated list;
   the machine is also a group of one under its own name.
   ``internet=`` is a comma-separated list of addresses or prefixes it may
   reach. ``lockdown`` turns on the host firewall, ``droplog`` logs what it
   drops.

``allow <group> <group>``
   Every member of the first group may talk to every member of the second,
   in both directions. ``allow g g`` connects the members of a group to
   each other.

``bypass <group> <bypass line>``
   Puts the line (``route``, ``in``, ``out``, ``in6`` or ``out6``, see
   :doc:`bypass-file`) in the bypass file of every member of the group.

Example
-------

.. code-block:: text

   switch switch1
   switch switch2
   agent  judge 10.0.0.1 judges internet=0.0.0.0/0
   agent  c001  10.0.1.1 contestants lockdown
   agent  c002  10.0.1.2 contestants lockdown
   allow  contestants judges
   bypass contestants route 192.0.2.10/32
   bypass contestants in -p tcp -s 198.51.100.0/24 --dport 22443 -j ACCEPT

Output
------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - File
     - Goes to
   * - ``auth.txt``
     - the conductor, ``/etc/vnet/auth.txt``
   * - ``switch.txt``
     - the conductor, ``/etc/vnet/switch.txt``
   * - ``machines/conductor.env``
     - the conductor, ``/etc/vnet/conductor.env``
   * - ``machines/<switch>.env``
     - that switch, ``/etc/vnet/switch.env``
   * - ``machines/<agent>.env``
     - that machine, ``/etc/vnet/agent.env``
   * - ``machines/<agent>.bypass``
     - that machine, ``/etc/vnet/bypass.conf`` (only when it has bypass
       lines)
   * - ``certs/ca.pem``
     - every machine, ``/etc/vnet/certs/``
   * - ``certs/<name>.pem``, ``certs/<name>.key``
     - that machine, ``/etc/vnet/certs/``
   * - ``certs/ca.key``
     - nowhere: keep it with the roster

The last line it prints sums up what it wrote:

.. code-block:: text

   wrote out: 2 switch(es), 3 agent(s), 2 allowed pair(s), 2 bypass file(s), certificates

Errors
------

``vnet-roster`` stops at the first error with ``roster line N: <reason>``:
an unknown line kind, a bad name or address, an agent given twice, an
``allow`` or ``bypass`` naming an unknown group, a malformed bypass route.
It also stops when two agents share an address or when the roster names no
switch.
