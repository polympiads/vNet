Prepare an event
================

A checklist for the weeks before an event. :doc:`../tutorials/first-event`
shows the commands once; this page says what to decide and what to test.

Size the network
----------------

Switches
   Each switch handles every packet of its machines. Plan the switches so
   that, with the largest one down, the others can carry its machines:
   with *n* switches each should be able to carry ``1/(n-1)`` of the
   event. Measure one switch with ``vNet-benchmarks`` on the hardware you
   will use rather than guessing.

Per-agent limit
   The default, 20000 packets per second and 200 Mbit/s per machine, is
   far above what a contestant needs and far below what a switch can
   carry. Lower it if a few machines must not be able to load a switch;
   raise it for a judge machine that serves many contestants
   (``VNET_AGENT_PPS``, ``VNET_AGENT_MBPS`` on the switches; the limit is
   the same for every machine of a switch).

File descriptors
   Each machine holds one socket on its switch and one on the conductor.
   The units raise ``LimitNOFILE`` to 65536; keep that if you run the
   programs another way.

Addresses
   Give each group its own range inside the overlay prefix (judges in
   ``10.0.0.0/24``, contestants in ``10.0.1.0/24``, ...): rules and logs
   become readable. No organizer machine and no network you need to reach
   may sit inside the overlay prefix; change it with
   ``VNET_OVERLAY_CIDR`` on every switch and agent if it collides.

Decide the rules
----------------

Write them as groups in the roster (:doc:`../reference/roster`):

* who may talk to whom (``allow``);
* which machines reach the internet, and which addresses
  (``internet=``); prefer the few addresses a machine needs over
  ``0.0.0.0/0``;
* which machines are locked down (``lockdown``), and what stays on their
  physical network (``bypass``): a VDI broker, the organizers' SSH
  access, a DNS server (:doc:`vdi-lockdown`).

Remember that a machine with no rule reaches nothing, not even DNS
through the overlay.

Prepare the machines
--------------------

* Install the same vNet build everywhere; mixed versions are refused or
  logged (``the two binaries parse it differently``).
* Organizer machines: fixed addresses, the conductor's port and the switch
  ports open between them and from the contestant network.
* Contestant image: the agent installed and enabled, ``/etc/vnet`` filled
  per machine. A machine must not be able to read another machine's key.
* Keep ``out/`` from ``vnet-roster`` on one organizer machine, with a
  copy offline. Keep ``ca.key`` out of every event machine.
* Check that the certificates are valid for the whole event
  (:doc:`certificates`).

Rehearse
--------

Run the full setup at least once with the real machines, or a room of
them, and walk through these:

1. Every machine joins: ``vnet-state /var/vnet-dump.bin /etc/vnet/auth.txt``
   lists nothing under *expected but not connected*.
2. The allowed pairs work and the others do not, across two switches.
3. A locked-down machine cannot reach its LAN, the internet directly, or
   any IPv6 address; what the bypass file allows still works.
4. Stop a switch: its machines move to the others within seconds.
5. Stop the conductor: the contest keeps working; start it again: the
   state comes back (:doc:`../concepts/recovery`).
6. Reboot a contestant machine: it joins again by itself.
7. Change a rule and reload the conductor: the change applies at once.

Keep the logs of the rehearsal: they are what a healthy event looks like,
and the reference when something looks wrong on the day.

On the day
----------

Before contestants arrive: start the conductor, then the switches, then
the machines; check ``vnet-state``; run the pings of the rehearsal. Then
see :doc:`during-event`.
