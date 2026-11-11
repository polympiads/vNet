Log messages
============

Every line starts with the part of the program that wrote it:
``[Conductor]``, ``[Switch]``, ``[Agent]``, ``[BlackBox]`` (the switch's
packet engine), ``[Config]`` (the switch config parser), ``[TUN]`` or
``[TLS]``. Errors go to standard error, the rest to standard output; both
end up in the journal. Words in angle brackets stand for values.

This page lists the lines an organizer meets. :doc:`../troubleshooting`
starts from the symptom instead.

Conductor
---------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Message
     - Meaning
   * - ``Listening on port <port>``
     - started; ready for switches and agents
   * - ``Switch config <path> (<n> bytes, fingerprint <fp>) will be sent to every switch``
     - the access list is pushed from here
   * - ``No VNET_CONFIG_PATH: switches read their own config files``
     - the switches must each have the same file
   * - ``Previous dump had <n> switch(es) and <m> agent(s)``
     - the conductor restarted; the state is rebuilt from the switches
   * - ``Switch registered: <name> @ <ip>:<port> (network=<net>)``
     - a switch joined; agents will be told to dial that address
   * - ``Config <fp> sent to <switch>`` / ``Switch <switch> applied config <fp>``
     - the access list reached a switch
   * - ``Agent <name> -> switch <switch>``
     - a machine was placed on a switch (join step 2)
   * - ``Agent <name> registered with IP <ip> on switch <switch>``
     - the join completed
   * - ``Agent <name> left <switch>``
     - the machine's switch link closed
   * - ``Agent reconnected: <name> on <switch>``
     - the machine's control link came back; its data path never stopped
   * - ``Connection closed: <name> (fd=<fd>, reason=hangup|error|epoll)``
     - a link closed; ``hangup`` is a clean close
   * - ``Rejected agent auth: <name>`` / ``Rejected switch auth: <name>``
     - unknown name, wrong role, or wrong key
   * - ``Rejected agent <name>: certificate role is '<role>'``
     - the machine presents a certificate made for another role
   * - ``No available switch for agent <name>``
     - no switch is registered for that machine's network
   * - ``Agent <name> registered again; closing fd <fd>``
     - a second join for a name already joined: normal after a switch
       loss, suspicious if repeated (two machines with one name)
   * - ``Agent <name> reconnected without a live switch``
     - the switch the machine was on is gone; the machine will join again
   * - ``Rejected switch <name>: its config (<fp>) differs from <other>'s (<fp>)``
     - switches reading their own files disagree
   * - ``Switch <name> runs config <fp>, not the one sent (<fp>); see its log``
     - the switch refused the pushed config
   * - ``Config mismatch: <a> and <b> apply different ACLs``
     - after a reload on one switch, two switches disagree
   * - ``Config reloaded, fingerprint <fp>; sent to <n> switch(es)``
     - a reload with a changed access list
   * - ``Config reloaded, unchanged (<fp>)``
     - a reload with no change in meaning
   * - ``Config reload failed; the previous config stays in force``
     - the file has an error; the line above names it
   * - ``Auth file reloaded: <n> record(s)`` / ``Auth file reload failed; ...``
     - the result of a reload of the auth file
   * - ``No MIP within 10s on fd <fd>``
     - a connection never identified itself (a port scan, a stuck client)
   * - ``Heartbeat timeout on fd <fd>``
     - nothing received for ``VNET_DEAD_SEC``: the peer froze or the
       network is cut
   * - ``No VNET_AUTH_FILE: every name and key would be accepted ...`` /
       ``TLS is off: ...``
     - refused to start without the auth file or TLS

Switch
------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Message
     - Meaning
   * - ``NAT 10.0.0.0/8 via vnet-out -> <if>``
     - internet forwarding is set up through ``<if>``
   * - ``No default route for NAT`` / ``Internet forwarding is not active``
     - no internet for this switch's machines
   * - ``Per-agent limit: <pps> pps, <mbps> Mbit/s (0 = none)``
     - the rate limit in force
   * - ``No VNET_CONFIG_PATH: waiting for the config from the conductor``
     - normal with a pushed config
   * - ``Registered with conductor as <name> on port <port>``
     - the conductor accepted this switch
   * - ``Config from the conductor applied, fingerprint <fp> (<n> bytes, <m> agents)``
     - the access list is in force
   * - ``Config from the conductor unchanged (<fp>)``
     - the conductor restarted and sent the same access list
   * - ``Config from the conductor (<fp>) did not parse; ...``
     - the pushed file has an error; the ``[Config]`` line above names it
   * - ``Re-announced <n> agent(s)``
     - after a conductor restart, this switch told it who is here
   * - ``Expecting agent <name>``
     - the conductor sent a token (join step 3)
   * - ``Agent <name> authenticated (fd=<fd>, ip=<ip>)``
     - the machine is attached (join step 5)
   * - ``Agent <name> authenticated again ...``
     - the same name joined again; the older link is replaced
   * - ``Rejected agent: unknown token (fd=<fd>)``
     - a token the conductor did not send here, or older than 60 s
   * - ``Agent <name> not found in config, rejecting``
     - the name is in the auth file but has no address in the access list
   * - ``Rejected agent <name>: no config received from the conductor yet``
     - the switch has no access list yet
   * - ``Route update: <ip> → <switch>``
     - a machine on another switch is reachable through that switch
   * - ``Route withdrawn: <ip>``
     - that machine left
   * - ``Dialing peer <switch>`` / ``Peer <switch> link up (dialed)`` /
       ``Peer <switch> connected inbound``
     - a link between two switches is being made, or is up
   * - ``Peer <switch> did not answer the dial``
     - the other switch's port cannot be reached from here
   * - ``Link to <switch> lost; <n> route(s) kept until it is back``
     - the link to another switch dropped while that switch is still
       registered; its routes return when the link is up again
   * - ``Peer switch disconnected: <switch>``
     - the conductor reported that switch gone
   * - ``Conductor link lost; agents keep their data path, retrying every 2s``
     - the conductor is down or unreachable; the contest is not affected
   * - ``Conductor did not answer the dial``
     - the conductor's port cannot be reached from here
   * - ``Stats: ...``
     - the counters (:doc:`../guides/monitoring`)
   * - ``Drop (<reason>)``
     - one dropped packet; only with ``VNET_DEBUG=1``

Agent
-----

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Message
     - Meaning
   * - ``Connecting to conductor <ip>:<port> ...``
     - a join starts
   * - ``Conductor <ip>:<port> not reachable``
     - no TCP connection; retried every 2 s
   * - ``TLS to conductor failed`` / ``TLS to switch failed``
     - the handshake failed; the ``[TLS]`` line says why
   * - ``No switch assignment from the conductor (rejected, no switch available, or no answer)``
     - the conductor refused or had no switch; its log says which
   * - ``Assigned to switch <switch> @ <ip>:<port>``
     - join step 3
   * - ``Switch <switch> not reachable (<n>/3)``
     - the switch's address cannot be reached from this machine
   * - ``Switch did not accept the connection``
     - the switch refused; its log says why
   * - ``Authenticated with switch <switch>, virtual IP: <ip>``
     - joined
   * - ``[TUN] Interface vnet-<name> up with IP <ip>/32``
     - the TUN is up
   * - ``<prefix> stays on the physical network via <gw> dev <if>``
     - a control address or bypass route was pinned
   * - ``Lockdown on: ...``
     - the host firewall is in force
   * - ``Entering event loop.``
     - the machine is on the overlay
   * - ``Switch link lost; joining again``
     - the switch went away; the agent joins another one
   * - ``Reconnecting control plane`` / ``Control plane reconnected``
     - the conductor link dropped and came back; traffic was not affected
   * - ``Virtual IP changed; reopening the TUN``
     - the access list gives this machine a new address
   * - ``Restored the default route a previous agent left behind: ...``
     - the last agent crashed; the host route was repaired
   * - ``No default route; control-plane pins are skipped``
     - the machine had no default route before joining
   * - ``Bypass file reloaded: <n> line(s)`` / ``Bypass file reload failed; ...``
     - the result of ``systemctl reload vnet-agent``
   * - ``Lockdown removed`` / ``Shutting down.``
     - the agent stopped and restored the host

Switch config parser
--------------------

``[Config] Loaded <n> agent(s), <m> with agent ACLs, <k> with internet
ACLs`` reports a file that parsed. The errors are listed in
:doc:`switch-config`.

TLS
---

``[TLS] Enabled, peer certificates required, role in OU checked`` at start.
``[TLS] handshake failed on fd <fd>`` and ``[TLS] connect`` are followed by
OpenSSL's reason (:doc:`../guides/certificates`).
