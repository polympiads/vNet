Environment variables
=====================

The programs take their settings from the environment; under systemd, from
the unit's ``EnvironmentFile`` (``/etc/vnet/conductor.env``,
``switch.env``, ``agent.env``). The *Programs* column says which program
reads a variable: **C** conductor, **S** switch, **A** agent.

An integer outside its range stops the program at start with
``[Config] NAME must be an integer between MIN and MAX, got 'VALUE'``.
A flag that is off by default is turned on by exactly ``1``; a flag that is
on by default (``VNET_TLS_ROLE_CHECK``, ``VNET_CONFIG_HASH_CHECK``,
``VNET_DEFAULT_VIA_TUN``) is turned off by exactly ``0``.

Command line
------------

.. code-block:: text

   conductor
   switch <name> [auth_key] <port>
   agent  <name> [auth_key]

Names are 1 to 10 characters for an agent, 1 to 32 for a switch, from
``[A-Za-z0-9_-]``. Give the key in the environment rather than on the
command line, where every local user can read it. The systemd units take
the name and port from ``VNET_AGENT_NAME``, ``VNET_SWITCH_NAME`` and
``VNET_SWITCH_PORT`` in the env file; the programs themselves do not read
these three.

Connection and identity
-----------------------

.. list-table::
   :header-rows: 1
   :widths: 26 9 20 45

   * - Variable
     - Programs
     - Default
     - Meaning
   * - ``CONDUCTOR_IP``
     - S A
     - required
     - Address or host name of the conductor.
   * - ``CONDUCTOR_PORT``
     - C S A
     - 5000 (C), required (S A)
     - TCP port of the conductor, 1 to 65535. The conductor listens on it on
       every address.
   * - ``VNET_AUTH_KEY``
     - S A
     -
     - This machine's key, as in the auth file. Takes precedence over
       ``VNET_AUTH_KEY_FILE`` and the command line.
   * - ``VNET_AUTH_KEY_FILE``
     - S A
     -
     - A file whose first line is the key.
   * - ``VNET_AUTH_FILE``
     - C
     - required
     - The auth file (:doc:`auth-file`). Read again on ``SIGHUP``.
   * - ``VNET_ADVERTISE_IP``
     - S
     - the address the conductor sees
     - Address or host name that agents and other switches dial to reach
       this switch. Set it when the switch is behind NAT or has several
       addresses.
   * - ``VNET_NETWORK``
     - S A
     - ``::internet`` (S), ``vnet.internal`` (A)
     - Network name used to place agents on switches
       (:doc:`../concepts/architecture`). A switch on ``::internet`` takes
       agents of every network.

TLS
---

.. list-table::
   :header-rows: 1
   :widths: 26 9 20 45

   * - Variable
     - Programs
     - Default
     - Meaning
   * - ``VNET_TLS``
     - C S A
     - off
     - ``1``: every link is mutual TLS (1.2 or later).
   * - ``VNET_TLS_CA``
     - C S A
     - required with TLS
     - The CA certificate, PEM.
   * - ``VNET_TLS_CERT``
     - C S A
     - required with TLS
     - This machine's certificate, PEM.
   * - ``VNET_TLS_KEY``
     - C S A
     - required with TLS
     - This machine's private key, PEM.
   * - ``VNET_TLS_ROLE_CHECK``
     - C S A
     - on
     - ``0``: do not check the role in the peer certificate's OU.
   * - ``VNET_ALLOW_INSECURE``
     - C S A
     - off
     - ``1``: start without TLS, and the conductor without an auth file.
       For a lab only.

Access list
-----------

.. list-table::
   :header-rows: 1
   :widths: 26 9 20 45

   * - Variable
     - Programs
     - Default
     - Meaning
   * - ``VNET_CONFIG_PATH``
     - C S
     -
     - The switch config (:doc:`switch-config`). On the conductor: sent to
       every switch, read again on ``SIGHUP``. On a switch: the config to
       start with, read again on ``SIGHUP``; a config from the conductor
       replaces it. A switch with neither waits for the conductor's.
   * - ``VNET_CONFIG_HASH_CHECK``
     - C
     - on
     - When the switches read their own files: refuse a switch whose config
       differs from the others'. ``0`` turns it off.
   * - ``VNET_OVERLAY_CIDR``
     - S A
     - ``10.0.0.0/8``
     - The overlay prefix. Must be the same on every switch and agent.

Switch
------

.. list-table::
   :header-rows: 1
   :widths: 26 9 20 45

   * - Variable
     - Programs
     - Default
     - Meaning
   * - ``VNET_AGENT_PPS``
     - S
     - 20000
     - Packets per second each machine may send, 0 to 100000000; 0 is no
       limit. Burst of one second.
   * - ``VNET_AGENT_MBPS``
     - S
     - 200
     - Mbit/s each machine may send, 0 to 100000; 0 is no limit. Burst of
       one second.
   * - ``VNET_STATS_SEC``
     - S
     - 60
     - Seconds between statistics lines, 0 to 86400; 0 turns them off
       (``SIGUSR1`` still prints them).
   * - ``VNET_DEBUG``
     - S
     - off
     - ``1``: log every dropped or held packet. Lab only.

Agent
-----

.. list-table::
   :header-rows: 1
   :widths: 26 9 20 45

   * - Variable
     - Programs
     - Default
     - Meaning
   * - ``VNET_DEFAULT_VIA_TUN``
     - A
     - on
     - The default route goes through the TUN. ``0``: only the overlay
       prefix does, and the machine keeps its own default route.
   * - ``VNET_LOCKDOWN``
     - A
     - off
     - ``1``: install the host firewall (:doc:`../guides/vdi-lockdown`).
   * - ``VNET_LOCKDOWN_LOG``
     - A
     - off
     - ``1``: log what the firewall drops to the kernel log.
   * - ``VNET_LOCKDOWN_ALLOW``
     - A
     -
     - Comma-separated prefixes ``a.b.c.d/n``, each the same as a ``route``
       line of the bypass file.
   * - ``VNET_BYPASS_FILE``
     - A
     -
     - The bypass file (:doc:`bypass-file`). Read again on ``SIGHUP``. A
       path that cannot be read stops the agent.
   * - ``VNET_STATE_DIR``
     - A
     - ``/run/vnet``
     - Where the agent saves the physical default route, as
       ``agent-<name>.route``.

Liveness and state
------------------

.. list-table::
   :header-rows: 1
   :widths: 26 9 20 45

   * - Variable
     - Programs
     - Default
     - Meaning
   * - ``VNET_HEARTBEAT_SEC``
     - C S A
     - 10
     - Seconds between heartbeats on every link, 1 to 3600.
   * - ``VNET_DEAD_SEC``
     - C S A
     - 30
     - Seconds without anything received before a link is closed, 1 to
       86400; must be larger than ``VNET_HEARTBEAT_SEC``. Use the same
       values on every machine.
   * - ``VNET_DUMP_PATH``
     - C, ``vnet-state``
     - ``/var/vnet-dump.bin``
     - Where the conductor writes its state dump.
   * - ``VNET_DUMP_SEC``
     - C
     - 60
     - Seconds between dumps, 1 to 86400.

Certificates tool
-----------------

``VNET_CERT_DAYS`` (default 400) is read by ``vnet-certs``: the validity of
the certificates it issues.
