Programs, signals and services
==============================

``cmake --install`` puts these in ``/usr/local/bin``:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Program
     - Role
   * - ``conductor``
     - the control plane (:doc:`../concepts/architecture`)
   * - ``switch``
     - a switch
   * - ``agent``
     - the agent of one machine
   * - ``vnet-state``
     - prints the conductor's state dump
   * - ``vnet-certs``
     - makes the CA and the certificates (:doc:`../guides/certificates`)
   * - ``vnet-roster``
     - generates an event's files from a roster (:doc:`roster`)

The switch and the agent need root, or ``CAP_NET_ADMIN`` and
``CAP_NET_RAW``: they open TUN devices and change routes and iptables.
They call ``ip``, ``iptables``, ``ip6tables`` and ``sysctl``, which must be
installed. The conductor needs no privilege beyond writing its dump.

Signals
-------

.. list-table::
   :header-rows: 1
   :widths: 16 28 28 28

   * - Signal
     - conductor
     - switch
     - agent
   * - ``SIGHUP``
     - reload the auth file and the switch config; send a changed config to
       every switch
     - reload its own config file, if ``VNET_CONFIG_PATH`` is set on it
     - reload the bypass file; rebuild routes and lockdown
   * - ``SIGUSR1``
     - write the state dump now
     - print the statistics line and one line per machine
     -
   * - ``SIGTERM``, ``SIGINT``
     - stop
     - stop
     - stop, restoring the routes and removing the lockdown

systemd units
-------------

``deploy/`` holds one unit per program, installed into
``/usr/local/lib/systemd/system``:

.. list-table::
   :header-rows: 1
   :widths: 22 30 48

   * - Unit
     - Environment
     - Notes
   * - ``vnet-conductor``
     - ``/etc/vnet/conductor.env``
     - ``LimitNOFILE=65536``
   * - ``vnet-switch``
     - ``/etc/vnet/switch.env``
     - runs ``switch ${VNET_SWITCH_NAME} ${VNET_SWITCH_PORT}``;
       ``LimitNOFILE=65536``
   * - ``vnet-agent``
     - ``/etc/vnet/agent.env``
     - runs ``agent ${VNET_AGENT_NAME}``; 10 seconds to stop and restore the
       host

All three restart two seconds after they exit (``Restart=always``) and map
``systemctl reload`` to ``SIGHUP``. Useful commands:

.. code-block:: console

   $ sudo systemctl enable --now vnet-agent      # start now and at boot
   $ sudo systemctl reload vnet-conductor        # SIGHUP
   $ sudo systemctl kill -s USR1 vnet-switch     # SIGUSR1
   $ journalctl -u vnet-switch -f                # follow the log

vnet-state
----------

.. code-block:: text

   vnet-state [dump file] [auth file]

Prints the conductor's state dump: when it was written, the switches with
their address, network and connection time, the agents with their virtual
address and switch, and the joins in progress. The dump defaults to
``VNET_DUMP_PATH``, then ``/var/vnet-dump.bin``. With the auth file it also
lists, under *expected but not connected*, every machine the file names
that is not in the dump. The dump is written every ``VNET_DUMP_SEC``
seconds; send ``SIGUSR1`` to the conductor first for a fresh one.

.. code-block:: text

   dump written 2026-11-11 19:06:34

   switches (3)
     switch3  172.18.0.3:6002  network=::internet  since 2026-11-11 19:05:07
     switch1  172.18.0.5:6000  network=::internet  since 2026-11-11 19:05:07
     switch2  172.18.0.4:6001  network=::internet  since 2026-11-11 19:05:07

   agents (2)
     agent1  10.0.1.1  on switch2  since 2026-11-11 19:05:09
     agent2  10.0.1.2  on switch1  since 2026-11-11 19:05:09

   expected but not connected (0)

It exits with 1 when the dump cannot be read or is not a dump.

vnet-certs
----------

.. code-block:: text

   vnet-certs init  <dir>                  make <dir>/ca.pem and <dir>/ca.key
   vnet-certs issue <dir> <role> <name>    make <dir>/<name>.pem and <dir>/<name>.key

``role`` is ``conductor``, ``switch`` or ``agent`` and is written in the
certificate's OU. ``VNET_CERT_DAYS`` sets the validity, 400 days by
default.
