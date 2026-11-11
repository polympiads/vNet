During an event
===============

What to watch, and how to change things once contestants are working.
Every command here leaves connected machines connected unless it says
otherwise.

Where the logs are
------------------

Each program writes to its systemd journal:

.. code-block:: console

   $ journalctl -u vnet-conductor -f          # on the conductor
   $ journalctl -u vnet-switch -f             # on a switch
   $ journalctl -u vnet-agent -n 50           # on a machine

:doc:`../reference/logs` explains the lines.

Who is connected
----------------

On the conductor:

.. code-block:: console

   $ sudo systemctl kill -s USR1 vnet-conductor     # write the dump now
   $ sudo vnet-state /var/vnet-dump.bin /etc/vnet/auth.txt
   dump written 2026-11-11 09:14:02

   switches (2)
     switch1  192.0.2.11:6000  network=::internet  since 2026-11-11 08:01:13
     switch2  192.0.2.12:6001  network=::internet  since 2026-11-11 08:01:14

   agents (3)
     c001  10.0.1.1  on switch1  since 2026-11-11 08:40:55
     c002  10.0.1.2  on switch2  since 2026-11-11 08:41:02
     judge  10.0.0.1  on switch1  since 2026-11-11 08:02:30

   expected but not connected (0)

The *joining* list appears only while joins are in progress.

A machine in *expected but not connected* is off, has no network, or is
refused: see :doc:`../troubleshooting`. A *since* time much later than the
others means the machine rejoined: look at its agent log. (After the
conductor restarts, every *since* is the time the machine reconnected to
it.)

What each switch is doing
-------------------------

.. code-block:: console

   $ sudo systemctl kill -s USR1 vnet-switch
   $ journalctl -u vnet-switch -n 20

The statistics line, and one line per machine, are explained in
:doc:`monitoring`.

Change who may talk to whom
---------------------------

Edit the access list on the conductor, then reload:

.. code-block:: console

   $ sudoedit /etc/vnet/switch.txt
   $ sudo systemctl reload vnet-conductor
   $ journalctl -u vnet-conductor -n 5
   [Conductor] Config reloaded, fingerprint 4be1c09a7f3d; sent to 2 switch(es)
   [Conductor] Switch switch1 applied config 4be1c09a7f3d
   [Conductor] Switch switch2 applied config 4be1c09a7f3d

The new rules apply to the next packet on every switch. If you generate
the files with ``vnet-roster``, edit the roster, run it again into the same
directory (keys are kept), and copy ``switch.txt`` and ``auth.txt`` to the
conductor before reloading.

If the log says ``Config reload failed; the previous config stays in
force``, the line before it names the line of the file that is wrong.

Add a machine
-------------

Add its key to ``/etc/vnet/auth.txt`` and its address and rules to
``/etc/vnet/switch.txt`` (or add it to the roster and generate again),
reload the conductor, install its files and start its agent. No other
machine is disturbed.

Remove a machine
----------------

Remove it from the auth file and reload: it can no longer join. A machine
already connected stays until its next join; stop its agent, or restart
the switch it is on, to remove it at once. Removing it from the access
list instead cuts its traffic at once while leaving it connected.

Let one machine out of lockdown
-------------------------------

On that machine: ``sudo systemctl stop vnet-agent``. The default route and
the firewall are put back as they were; the machine is off the overlay.
To open only one address, add a line to its bypass file and run ``sudo
systemctl reload vnet-agent`` (:doc:`vdi-lockdown`).

Restart a program
-----------------

Any program can be restarted at any time; :doc:`../concepts/recovery`
says what the machines see. In short: restarting the conductor is
invisible to the contest; restarting a switch moves its machines to the
other switches; restarting an agent cuts its machine for a few seconds.

Do not
------

* Edit a file and forget to reload: nothing changes until the reload,
  and the next restart then applies a change you no longer remember.
* Give two machines the same name or the same env file: they keep
  pushing each other off (:doc:`../concepts/recovery`).
* Run two conductors. Agents and switches would split between them.
