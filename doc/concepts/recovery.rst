Failures and recovery
=====================

Every program can stop at any moment: a crash, a reboot, a cable pulled.
This page says what the machines see in each case and what brings the
network back. In each case nothing has to be done by hand beyond starting
the program again, which systemd does after two seconds
(``Restart=always``).

How a dead link is noticed
--------------------------

Every link carries a heartbeat every 10 seconds (``VNET_HEARTBEAT_SEC``).
A link that has received nothing for 30 seconds
(``VNET_DEAD_SEC``) is closed with ``Heartbeat timeout on fd N``. A link
that closes cleanly (the process stopped, the machine rebooted) is noticed
at once.

The conductor stops
-------------------

* Packets keep flowing. Switches and agents keep their links to each
  other; the access list stays in force on every switch.
* New machines cannot join, and a machine whose switch link drops cannot
  rejoin, until the conductor is back.
* Switches log ``Conductor link lost; agents keep their data path,
  retrying every 2s``; agents log ``Reconnecting control plane``.

When the conductor starts again, it knows nothing of the network: the state
dump it reads is only printed (``Previous dump had N switch(es) and M
agent(s)``). The switches reconnect and re-announce their agents
(``Re-announced N agent(s)``), the conductor sends them the config again
(``Config from the conductor unchanged``), and the agents reconnect their
control link (``Control plane reconnected``). Routes are published again as
the switches re-announce. Within a few seconds the conductor's view is
complete again, which ``vnet-state`` shows.

A switch stops
--------------

* The machines attached to it lose their data path. Each agent logs
  ``Switch link lost; joining again``, asks the conductor for a switch, and
  is placed on another one that is up. Its TUN stays up and keeps its
  virtual address, so applications see a pause, not a new interface;
  TCP connections usually survive a pause of a few seconds.
* The other switches drop their routes through the lost switch and learn
  the new ones as the machines rejoin.
* When the switch starts again, it registers and receives new joins. The
  machines already moved stay where they are; vNet does not move machines
  back.

Plan for this: run enough switches that the others can take the load of
the biggest one (:doc:`../guides/prepare-event`).

An agent stops
--------------

* Its machine is off the overlay. The switch tells the conductor, and the
  conductor withdraws the route from every switch.
* Stopping cleanly (``systemctl stop``) puts the default route back,
  removes the pins and the lockdown chains: the machine is on its physical
  network as before.
* If the agent crashed, the default route may still point into a TUN that
  no longer exists, and the lockdown chains are still there. The machine
  is cut off until the agent starts again; systemd restarts it, and the
  new agent finds the physical gateway in
  ``/run/vnet/agent-<name>.route`` (``Restored the default route a
  previous agent left behind``). To free a machine by hand, see
  :doc:`../troubleshooting`.

A link between two switches drops
---------------------------------

Packets between machines on those two switches are held for up to 5
seconds and then dropped while the link is down (they count under
``waited-for-route``, not ``dropped``). Machines on the same switch and the
internet are not affected. The switches dial each other again every 2
seconds and give up on an attempt after 5 seconds, so the link comes back
on its own (``Peer <name> link up``).

.. warning::

   In this version the routes through the link are not restored when it
   comes back: the statistics line shows ``peers=1`` but fewer ``routes``
   than before, and the machines on the two switches still cannot reach
   each other. Restart the conductor (``systemctl restart
   vnet-conductor``): the switches re-announce their machines and every
   route is published again within seconds, without disturbing the
   machines. Restarting the agents concerned also works.

Two machines use the same name
------------------------------

The newer join wins: the conductor closes the older control link, and the
switch replaces the older data link. The two machines then take turns,
each one rejoining after the other. ``vnet-state`` shows the name moving
between switches, and both machines' logs show ``Switch link lost``. Give
each machine its own name and key.

The state dump
--------------

Every 60 seconds (``VNET_DUMP_SEC``), and on ``SIGUSR1``, the conductor
writes its view to ``/var/vnet-dump.bin`` (``VNET_DUMP_PATH``): the
switches, the agents and where they are, and the joins in progress.
``vnet-state`` prints it (:doc:`../reference/commands`). The dump is for
people, not for recovery: the conductor rebuilds its state from the
switches, never from the file.
