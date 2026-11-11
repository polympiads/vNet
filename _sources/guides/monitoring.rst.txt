Monitor the network
===================

vNet has three sources of information: the conductor's state dump, the
switches' counters, and the logs. None needs extra software.

The state dump
--------------

The conductor writes its view of the network every 60 seconds
(``VNET_DUMP_SEC``) and on ``SIGUSR1``, to ``/var/vnet-dump.bin``
(``VNET_DUMP_PATH``). Read it with ``vnet-state``; given the auth file, it
also lists the machines that should be there and are not:

.. code-block:: console

   $ sudo systemctl kill -s USR1 vnet-conductor
   $ sudo vnet-state /var/vnet-dump.bin /etc/vnet/auth.txt

The output is described in :doc:`during-event` and
:doc:`../reference/commands`. ``vnet-state`` reads a copied file as well,
so the dump can be watched from another machine.

The switch counters
-------------------

Every switch prints one statistics line every 60 seconds
(``VNET_STATS_SEC``; 0 turns it off), and on ``SIGUSR1`` the same line
followed by one line per machine:

.. code-block:: console

   $ sudo systemctl kill -s USR1 vnet-switch
   $ journalctl -u vnet-switch -n 5
   [Switch] Stats: agents=1 routes=1 local=16 to-switch=16 to-internet=2 waited-for-route=8 dropped=6 acl=6 link-full-drops=0 tun-write-errors=0 peers=1 pending-tokens=0 conductor=up
   [Switch] Agents:
   agent1 10.0.1.1 fd=7 in=30/2520B out=16/1344B dropped=4

The counters count since the switch started.

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Field
     - Meaning
   * - ``agents``
     - machines attached to this switch now
   * - ``routes``
     - machines on other switches this switch knows how to reach
   * - ``local``
     - packets delivered to a machine of this switch
   * - ``to-switch``
     - packets sent to another switch
   * - ``to-internet``
     - packets sent to the internet
   * - ``waited-for-route``
     - packets held because the destination's route was not known yet
       (:doc:`../concepts/data-plane`)
   * - ``dropped``
     - packets dropped, all reasons together; then one field per reason
       that occurred: ``malformed``, ``unknown-source``, ``spoof``,
       ``rate-limited``, ``acl``, ``unreachable``
   * - ``link-full-drops``
     - packets dropped because the link to their destination was full
   * - ``tun-write-errors``
     - packets that could not be written to the internet interface
   * - ``peers``
     - links to other switches that are up or being dialed
   * - ``pending-tokens``
     - joins announced by the conductor that have not completed yet
   * - ``conductor``
     - ``up`` or ``down``: the link to the conductor

And per machine:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Field
     - Meaning
   * - name, address
     - the machine and its virtual address
   * - ``fd``
     - the socket of its link, as in the ``Connection closed`` lines
   * - ``in``
     - packets/bytes received from the machine
   * - ``out``
     - packets/bytes delivered to the machine
   * - ``dropped``
     - packets from this machine that were dropped

What healthy looks like
-----------------------

* ``conductor=up`` on every switch, and ``peers`` equal to the number of
  other switches that hold machines (a switch links to another one when
  it first learns a route through it).
* ``routes`` on each switch equal to the number of machines on the other
  switches (``vnet-state`` gives both).
* ``pending-tokens`` back to 0 a few seconds after machines join.
* ``acl`` drops growing slowly: machines trying what the rules forbid
  (software updates, telemetry, a contestant exploring). A sudden rise from
  one machine is worth a look.
* No ``spoof``, ``unknown-source`` or ``malformed``: these mean a machine
  sends packets it should not be able to make, or a broken agent.
* ``rate-limited`` only on machines you expect to be busy.
* ``link-full-drops`` and ``tun-write-errors`` at 0. A growing value means
  a link or the uplink cannot keep up.

To watch the counters over time, collect the ``[Switch] Stats:`` lines
from the journal of every switch; each line is self-contained.

Debug output
------------

``VNET_DEBUG=1`` on a switch logs every dropped packet with its reason,
every packet held for a route, and every write that failed. It is meant
for a lab: on a busy switch it writes a line per packet. Set it in
``/etc/vnet/switch.env`` and restart the switch.
