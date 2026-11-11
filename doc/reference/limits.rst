Limits and timings
==================

Fixed values in the code, and the variables that change the others. Values
in *Variable* can be set per program (:doc:`environment`); the others need
a rebuild.

Timings
-------

.. list-table::
   :header-rows: 1
   :widths: 40 15 20 25

   * - What
     - Value
     - Where
     - Variable
   * - Heartbeat interval on every link
     - 10 s
     - C S A
     - ``VNET_HEARTBEAT_SEC``
   * - Silence before a link is closed
     - 30 s
     - C S A
     - ``VNET_DEAD_SEC``
   * - Time for a new connection to identify itself
     - 10 s
     - C S
     -
   * - Validity of a join token
     - 60 s
     - C S
     -
   * - Agent: delay between join attempts
     - 2 s
     - A
     -
   * - Agent: timeout of one join step (connect, or one reply)
     - 10 s
     - A
     -
   * - Agent: tries on its assigned switch before asking the conductor
       again
     - 3
     - A
     -
   * - Switch: delay between attempts to reach the conductor or a peer
     - 2 s
     - S
     -
   * - Switch: timeout of one connection attempt
     - 5 s
     - S
     -
   * - Switch: how long a packet waits for its route
     - 5 s
     - S
     -
   * - Switch: interval of the statistics line
     - 60 s
     - S
     - ``VNET_STATS_SEC``
   * - Conductor: interval of the state dump
     - 60 s
     - C
     - ``VNET_DUMP_SEC``
   * - systemd: delay before a restart
     - 2 s
     - units
     - ``RestartSec``

Sizes
-----

.. list-table::
   :header-rows: 1
   :widths: 40 20 15 25

   * - What
     - Value
     - Where
     - Variable
   * - Agent name
     - 1 to 10 characters
     - all
     -
   * - Switch name
     - 1 to 32 characters
     - all
     -
   * - Largest frame payload
     - 65994 bytes
     - all
     -
   * - Write buffer of one link
     - 2 MiB
     - all
     -
   * - Packets read from one connection before serving the next
     - 64
     - all
     -
   * - Connections accepted per loop turn
     - 64
     - C S
     -
   * - Listen backlog
     - 1024
     - C S
     -
   * - Packets held per destination waiting for a route
     - 16
     - S
     -
   * - Piece of a pushed config
     - 60000 bytes
     - C
     -
   * - Largest switch config a switch accepts
     - 16 MiB
     - S
     -
   * - Packets per second per machine
     - 20000
     - S
     - ``VNET_AGENT_PPS``
   * - Bandwidth per machine
     - 200 Mbit/s
     - S
     - ``VNET_AGENT_MBPS``
   * - Open files
     - 65536
     - units
     - ``LimitNOFILE``

Addresses and ports
-------------------

.. list-table::
   :header-rows: 1
   :widths: 40 30 30

   * - What
     - Value
     - Variable
   * - Conductor port
     - 5000
     - ``CONDUCTOR_PORT``
   * - First switch port (``vnet-roster``)
     - 6000
     - ``--switch-port``
   * - Overlay prefix
     - ``10.0.0.0/8``
     - ``VNET_OVERLAY_CIDR``
   * - Agent TUN
     - ``vnet-<name>``
     -
   * - Switch internet TUN
     - ``vnet-out``, ``192.0.2.1/32``
     -
   * - State dump
     - ``/var/vnet-dump.bin``
     - ``VNET_DUMP_PATH``
   * - Agent route file
     - ``/run/vnet/agent-<name>.route``
     - ``VNET_STATE_DIR``
