C++ API reference
=================

The classes and functions of the vNet sources, generated from the comments
in ``include/`` by Doxygen and Breathe. This part is for people who change
vNet; organizers do not need it.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Module
     - Contents
   * - :doc:`blackbox`
     - ``vnet::blackbox``: the switch's packet engine. The access list
       (``Config``), the machines attached to a switch
       (``AgentRegistry``), the routes to other switches
       (``RoutingTable``), the rate limiter and the decision for each
       packet (``BlackBox``). No I/O.
   * - :doc:`netqueue`
     - ``vnet::netqueue``: the event loop shared by the three programs.
       Non-blocking sockets and TUN devices in one epoll loop, framing,
       TLS sessions, write buffers.
   * - :doc:`protocol`
     - ``vnet::protocol``: the frame header, the message type numbers and
       the dispatch interface each program implements.
   * - :doc:`tls`
     - ``vnet::tls``: the TLS context, handshakes and the role check.
   * - :doc:`common`
     - Free functions shared by the programs: sockets, TUN devices, the
       bypass file, system helpers.

.. toctree::
   :hidden:

   blackbox
   netqueue
   protocol
   tls
   common
