Wire protocol
=============

Organizers do not need this page to run an event. It is here to read a
packet capture, or to check what a version change touches.

Framing
-------

Every link is one TCP connection, wrapped in TLS when ``VNET_TLS=1``. It
carries frames:

.. code-block:: text

   +----------------------+------------------+---------------------------+
   | payload size, uint32 | type, uint16     | payload (size bytes)      |
   | network byte order   | network order    |                           |
   +----------------------+------------------+---------------------------+

The header is 6 bytes. A payload is at most 65994 bytes (a 66000-byte
buffer less the header); a frame that announces more closes the
connection. ``IPV4_RAW`` carries an IPv4 packet as it is; ``HEARTBEAT``
has no payload; every other type carries a protobuf message defined in
``proto/mip.proto``.

Message types
-------------

Numbers are fixed for the life of the protocol; a removed number is not
reused.

.. list-table::
   :header-rows: 1
   :widths: 8 28 20 44

   * - Type
     - Name
     - Direction
     - Purpose
   * - 0
     - ``HEARTBEAT``
     - every link
     - keeps the link alive
   * - 1
     - ``SWITCH_MIP``
     - switch to conductor
     - registration: name, key, port, network, advertised address, config
       fingerprint
   * - 2
     - ``AGENT_MIP``
     - agent to conductor
     - join: name, key, network
   * - 3
     - ``CONNECT_TO_SWITCH``
     - conductor to agent
     - the switch to use and a one-time token
   * - 4
     - ``AGENT_CONNECTION_TOKEN``
     - conductor to switch
     - the agent name that goes with the token
   * - 5
     - ``AUTH_CONNECT_TO_SWITCH``
     - agent to switch
     - the token
   * - 6
     - ``CONNECTION_ACCEPTED``
     - switch to agent
     - the virtual address
   * - 7
     - ``AGENT_MRP``
     - agent to conductor
     - reconnects the control link of an agent that kept its switch
   * - 8, 9
     - unused
     -
     -
   * - 10
     - ``IPV4_RAW``
     - agent and switch, switch and switch
     - one IPv4 packet
   * - 11
     - ``AGENT_REGISTERED``
     - switch to conductor
     - an agent attached to this switch
   * - 12
     - ``SWITCH_ROUTE_UPDATE``
     - conductor to switch
     - the switch that holds a virtual address
   * - 13
     - ``SWITCH_DISCONNECTED``
     - conductor to switch
     - a switch left; drop its routes
   * - 14
     - ``SWITCH_HELLO``
     - switch to switch
     - opens a peer link, with the pair's token
   * - 15
     - ``AGENT_UNREGISTERED``
     - switch to conductor, conductor to switch
     - an agent left; withdraw its route
   * - 16
     - ``PEER_TOKEN``
     - conductor to switch
     - the token a pair of switches uses to link
   * - 17
     - ``SWITCH_CONFIG``
     - switch to conductor
     - the fingerprint of the config the switch applies
   * - 18
     - ``CONFIG_PUSH``
     - conductor to switch
     - one piece of the switch config (at most 60000 bytes of text), with
       its index, a last-piece flag and the fingerprint

The order of the join messages is shown in :doc:`../concepts/joining`.

Rules a program enforces
------------------------

* A connection must identify itself (``SWITCH_MIP``, ``AGENT_MIP``,
  ``AGENT_MRP``, ``AUTH_CONNECT_TO_SWITCH`` or ``SWITCH_HELLO``) within 10
  seconds, or it is closed.
* A second identification on an identified connection is refused.
* A switch accepts tokens, routes and configs only on its conductor link.
* A switch accepts ``IPV4_RAW`` only from an authenticated agent or a peer
  switch.
* With TLS, the role in the peer certificate must match the role the
  connection claims.
