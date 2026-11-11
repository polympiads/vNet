Troubleshooting
===============

Start from what you see. Each case gives the log lines that identify it,
the cause, and the fix. The log lines are copied from real runs. Logs are
read with ``journalctl -u vnet-<program>``; ``vnet-state`` is described in
:doc:`guides/during-event`.

.. contents:: On this page
   :local:
   :depth: 1

A machine does not join
-----------------------

``vnet-state`` lists it under *expected but not connected*. Read its agent
log first: the last lines say how far the join went
(:doc:`concepts/joining`).

The agent cannot reach the conductor
   .. code-block:: text

      [Agent] Connecting to conductor 192.0.2.5:5000 ...
      [Agent] Conductor 192.0.2.5:5000 not reachable

   The conductor is down, or TCP 5000 is blocked between the machine and
   the conductor, or ``CONDUCTOR_IP`` is wrong. From the machine:
   ``nc -zv 192.0.2.5 5000``. With lockdown on, the agent opens the
   conductor's port itself; check that ``CONDUCTOR_IP`` is the address the
   machine really dials.

The conductor refuses the machine
   .. code-block:: text

      [Agent] No switch assignment from the conductor (rejected, no switch available, or no answer)

   The conductor's log says which:

   ``[Conductor] Rejected agent auth: c001``
      The name is not in the auth file, is listed as a switch, or the key
      differs. Compare ``VNET_AUTH_KEY`` in ``/etc/vnet/agent.env`` with the
      line in ``/etc/vnet/auth.txt``. After editing the auth file, reload
      the conductor.

   ``[Conductor] Rejected agent c001: certificate role is 'switch'``
      The machine has a certificate made for another role, often a copy
      of the wrong file. Issue an agent certificate for it
      (:doc:`guides/certificates`).

   ``[Conductor] No available switch for agent c001``
      No switch is registered, or none for the machine's network. Check
      that the switches are up (``vnet-state``) and that ``VNET_NETWORK``
      is the same on the machine and a switch, or unset.

The TLS handshake fails
   .. code-block:: text

      [TLS] connect
      ...:SSL routines:tls_post_process_server_certificate:certificate verify failed:...
      [Agent] TLS to conductor failed

   The machine and the conductor do not share a CA, or a certificate has
   expired. See :doc:`guides/certificates`.

The machine cannot reach its switch
   .. code-block:: text

      [Agent] Assigned to switch switch2 @ 192.0.2.12:6001
      [Agent] Switch switch2 not reachable (1/3)

   The switch port is blocked from this machine, or the address is wrong:
   the conductor gives agents the address it sees the switch connect from.
   If the switch is behind NAT or has several addresses, set
   ``VNET_ADVERTISE_IP`` on the switch. After three tries the agent asks
   the conductor again, which may pick another switch.

The switch refuses the machine
   .. code-block:: text

      [Agent] Switch did not accept the connection

   The switch's log says why:

   ``[Switch] Agent c003 not found in config, rejecting``
      The machine is in the auth file but has no address in the access
      list. Add ``c003 <address>`` to the switch config and reload the
      conductor.

   ``[Switch] Rejected agent c003: no config received from the conductor yet``
      The switch has no access list: the conductor has no
      ``VNET_CONFIG_PATH`` and the switch no file of its own, or the
      pushed file did not parse (see below).

   ``[Switch] Rejected agent: unknown token (fd=12)``
      The token expired (60 s) or was meant for another switch: the agent
      was very slow between the two steps, or two machines share a name.

The agent stops at once
   ``[Agent] CONDUCTOR_IP and CONDUCTOR_PORT must be set``,
   ``[Agent] No auth key: set VNET_AUTH_KEY or VNET_AUTH_KEY_FILE``,
   ``[Agent] TLS is off: ...``, ``[Agent] Cannot open bypass file ...``:
   the env file is incomplete, or the unit cannot read it. ``systemctl
   status vnet-agent`` shows the last lines.

Two machines cannot reach each other
------------------------------------

Both are connected, but a ping between them gets no answer.

The rules do not allow it
   The access list has no line connecting them. On their switches, the
   ``acl`` counter rises with each attempt (``systemctl kill -s USR1
   vnet-switch``). Check ``/etc/vnet/switch.txt`` on the conductor for a
   line with both names, add it, reload the conductor.

The destination is not where you think
   ``vnet-state`` gives each machine's virtual address. An address that is
   in no address line counts as internet and is dropped as ``acl``.

They are on different switches and the switches are not linked
   On the sender's switch, ``routes`` is lower than the number of machines
   on the other switches, and ``waited-for-route`` rises. Look for:

   .. code-block:: text

      [Switch] Peer switch1 did not answer the dial

   The switch ports must be reachable between switches, not only from
   the machines. If the link was up and dropped earlier
   (``Heartbeat timeout``, ``Connection closed: switch1``), the routes
   through it are not restored by themselves in this version: restart the
   conductor (:doc:`concepts/recovery`).

The receiving machine filters it
   The switch delivered it (``local`` or ``to-switch`` rises) but the
   machine does not answer: its own firewall, or the application listens
   on another address. Lockdown accepts everything on the TUN.

A machine cannot reach the internet
-----------------------------------

The machine has no rule
   Without a ``::internet:`` line for that address or a prefix holding it,
   the packet is dropped as ``acl``.

The switch has no internet
   The switch logs at start ``No default route for NAT`` or ``Internet
   forwarding is not active`` instead of ``NAT 10.0.0.0/8 via vnet-out ->
   eth0``. Give the switch a default route and restart it.
   ``tun-write-errors`` rising points the same way.

Names do not resolve
   An address works but a name does not: DNS is blocked. Allow the
   resolver (:doc:`guides/vdi-lockdown`).

A machine lost its network after the agent started
--------------------------------------------------

The agent moves the default route into the TUN and, with lockdown, closes
the physical network. Everything not allowed by the rules or the bypass
file goes away by design: the LAN, a VDI session, SSH from the
organizers, DNS, NTP.

* Read ``[Agent] Lockdown on: ...`` and the ``stays on the physical
  network`` lines: what is listed is what remains.
* With ``droplog``, ``journalctl -k -g vnet-drop`` shows what is dropped.
* Add a ``route`` or ``in`` line to the bypass file and
  ``systemctl reload vnet-agent``.

If you cannot reach the machine at all, use its console:
``systemctl stop vnet-agent`` restores everything. If the agent is gone,
free it by hand (:doc:`guides/vdi-lockdown`).

Machines keep dropping and rejoining
------------------------------------

Two machines share a name
   The conductor logs ``Agent c001 registered again; closing fd N`` over
   and over, and ``vnet-state`` shows ``c001`` on a different switch each
   time. Two machines have the same env file: give each its own.

The network loses packets
   ``Heartbeat timeout on fd N`` in the switch or agent logs, without a
   process stopping. Something between them drops traffic for
   ``VNET_DEAD_SEC`` seconds. Raise ``VNET_DEAD_SEC`` (and keep it above
   ``VNET_HEARTBEAT_SEC``) on every program if the network is known to
   stall, and look at the physical network.

A switch restarts in a loop
   ``systemctl status vnet-switch`` and its log: a missing variable, a bad
   config file, or a port already in use.

A switch is refused, or the access list differs
-----------------------------------------------

``[Conductor] Rejected switch switch2: its config (...) differs from switch1's (...)``
   The switches read their own files and the files differ. Copy the same
   file to every switch, or let the conductor push it
   (``VNET_CONFIG_PATH`` on the conductor).

``[Conductor] Switch switch2 runs config ..., not the one sent (...); see its log``
   The switch refused the pushed file. Its log says ``Config from the
   conductor (...) did not parse`` with the ``[Config]`` line naming the
   error. If it says ``the two binaries parse it differently``, the
   conductor and the switch run different versions of vNet: install the
   same build on both.

``[Conductor] Rejected switch auth: switch2``
   Wrong name or key for the switch, as for an agent.

A change did not take effect
----------------------------

* A reload was not sent: the files are read at start and on reload only.
* The reload failed: ``Config reload failed; the previous config stays in
  force`` or ``Auth file reload failed``; the line before names the error.
* The change only applies at the next join: a new virtual address, a
  removed or changed key (:doc:`concepts/configuration`).
* A switch has ``VNET_CONFIG_PATH`` of its own and was reloaded: it reads
  its own file, then the conductor's next push replaces it.

The network is slow or loses packets under load
-----------------------------------------------

``rate-limited`` rising
   A machine sends more than ``VNET_AGENT_PPS`` or ``VNET_AGENT_MBPS``.
   Expected from a judge serving many contestants: raise the limit on the
   switch that holds it.

``link-full-drops`` rising
   A link cannot keep up: the destination's switch link or the machine's
   own. Look at the network between switches, or spread machines over more
   switches.

One switch carries most machines
   Machines stay where they were placed after a switch comes back, so a
   restart leaves the others loaded. Placement is round-robin, not by
   load: restarting the agents of some machines spreads them over the
   switches in turn.

The conductor does not start
----------------------------

``No VNET_AUTH_FILE: every name and key would be accepted.``
   Set ``VNET_AUTH_FILE``. ``VNET_ALLOW_INSECURE=1`` is for a lab only.

``TLS is off: keys and routes would cross the network in the clear.``
   Set ``VNET_TLS=1`` and the TLS files.

``Auth file line N: ...`` or ``Config file ... did not parse``
   Fix the line named (:doc:`reference/auth-file`,
   :doc:`reference/switch-config`).

``bind: Address already in use``
   Another conductor runs on that port.

When nothing here fits
----------------------

Collect, for the machine and its switch: ``vnet-state`` with the auth
file, the switch's statistics after ``SIGUSR1``, the last 100 lines of the
three logs around the time of the problem, and the configuration files
with the keys removed. Reproduce in the Docker lab
(:doc:`tutorials/docker`) if you can, with ``VNET_DEBUG=1`` on the
switches.
