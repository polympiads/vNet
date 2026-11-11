Try vNet with Docker
====================

This tutorial runs a whole vNet deployment on one machine with Docker
Compose: one conductor, three switches and two agents, with TLS on every
link. It takes about five minutes, most of it building the images. At the
end you will have seen a machine join, the access list at work, a rule
changed live, a switch fail and the conductor restart.

What you need
-------------

* A Linux machine with Docker Engine and the Compose plugin. The switches
  and agents need ``/dev/net/tun`` and the ``NET_ADMIN`` capability, which
  Docker provides on Linux.
* ``git`` and ``openssl``.

1. Get the code and make the certificates
-----------------------------------------

.. code-block:: console

   $ git clone https://github.com/polympiads/vNet.git
   $ cd vNet
   $ ./docker/certs.sh
   wrote /home/you/vNet/docker/certs/ca.pem and /home/you/vNet/docker/certs/ca.key
   ...
   certificates are in /home/you/vNet/docker/certs

``docker/certs.sh`` makes a certificate authority and one certificate per
container, each with its role (``conductor``, ``switch`` or ``agent``) in
the certificate. The directory is ignored by git.

2. Start everything
-------------------

.. code-block:: console

   $ cd docker
   $ docker compose up -d --build
   $ docker compose ps --format 'table {{.Name}}\t{{.Status}}'
   NAME             STATUS
   vnet-agent       Up 8 seconds
   vnet-agent2      Up 8 seconds
   vnet-conductor   Up 19 seconds (healthy)
   vnet-switch1     Up 13 seconds (healthy)
   vnet-switch2     Up 13 seconds (healthy)
   vnet-switch3     Up 13 seconds (healthy)

The setup is described by three files in ``docker/``:

``auth.txt``
   The machines the conductor accepts, with their keys.

``switch.txt``
   The access list: ``agent1`` is ``10.0.1.1``, ``agent2`` is
   ``10.0.1.2``, the two may talk to each other, and each may reach
   ``8.8.8.8``.

``docker-compose.yml``
   The six containers and their environment.

Look at how the first agent joined:

.. code-block:: console

   $ docker logs vnet-agent
   [TLS] Enabled, peer certificates required, role in OU checked
   [Agent] Connecting to conductor vnet-conductor:5000 ...
   [Agent] Assigned to switch switch2 @ 172.18.0.4:6001
   [Agent] Authenticated with switch switch2, virtual IP: 10.0.1.1
   [TUN] Interface vnet-agent1 up with IP 10.0.1.1/32
   [Agent] Entering event loop.

The agent asked the conductor for a switch, received one together with a
one-time token, presented the token to the switch and got its virtual
address. :doc:`../concepts/joining` explains why it happens in that order.
Your switch names and addresses may differ: the conductor hands out
switches in turn.

.. note::

   The conductor and switch logs show ``[TLS] handshake failed`` every few
   seconds. Those are Docker's health checks, which open a TCP connection
   to check the port and close it without speaking TLS. They are harmless.
   The ``sysctl ... Read-only file system`` lines of the switches are
   harmless too: Compose sets those values from outside the container.

3. Send traffic through the overlay
-----------------------------------

The two agents may talk to each other, so a ping crosses the overlay, from
``agent1`` on one switch to ``agent2`` on another:

.. code-block:: console

   $ docker exec vnet-agent ping -c 3 10.0.1.2
   64 bytes from 10.0.1.2: icmp_seq=1 ttl=64 time=0.782 ms
   64 bytes from 10.0.1.2: icmp_seq=2 ttl=64 time=1.32 ms
   64 bytes from 10.0.1.2: icmp_seq=3 ttl=64 time=1.27 ms

The agent replaced the container's default route with its TUN interface,
so every IPv4 packet of the container now goes through the switch:

.. code-block:: console

   $ docker exec vnet-agent ip route
   default dev vnet-agent1 scope link
   10.0.0.0/8 dev vnet-agent1 scope link
   172.18.0.0/16 dev eth0 proto kernel scope link src 172.18.0.7

``8.8.8.8`` is allowed, so the switch forwards it to the internet; whether
the reply arrives depends on whether your machine can reach that address.
``1.1.1.1`` is not in the access list, so the switch drops it:

.. code-block:: console

   $ docker exec vnet-agent ping -c 2 -W 2 1.1.1.1
   2 packets transmitted, 0 received, 100% packet loss, time 1028ms

Ask the switch what it did. ``SIGUSR1`` prints its counters and one line
per agent:

.. code-block:: console

   $ docker kill -s USR1 vnet-switch2
   $ docker logs --since 5s vnet-switch2 2>&1 | grep -v TLS
   [Switch] Stats: agents=1 routes=1 local=3 to-switch=3 to-internet=2 waited-for-route=0 dropped=4 acl=4 link-full-drops=0 tun-write-errors=0 peers=2 pending-tokens=0 conductor=up
   [Switch] Agents:
   agent1 10.0.1.1 fd=7 in=7/588B out=3/252B dropped=2

``acl=4`` counts the packets the access list refused.
:doc:`../reference/logs` explains every counter.

4. Change a rule while everything runs
--------------------------------------

Allow ``agent1`` to reach ``1.1.1.1`` and reload the conductor:

.. code-block:: console

   $ echo 'agent1  ::internet:1.1.1.1' >> switch.txt
   $ docker kill -s HUP vnet-conductor
   $ docker logs --since 5s vnet-conductor 2>&1 | grep -v TLS
   [Conductor] Auth file reloaded: 5 record(s)
   [Config] Loaded 2 agent(s), 2 with agent ACLs, 2 with internet ACLs
   [Conductor] Config reloaded, fingerprint fee95658ab07; sent to 3 switch(es)
   [Conductor] Switch switch2 applied config fee95658ab07
   [Conductor] Switch switch3 applied config fee95658ab07
   [Conductor] Switch switch1 applied config fee95658ab07

The conductor sent the new access list to every switch, and each switch
confirmed it with the same fingerprint. The rule applies to the next
packet; no machine reconnected.

.. note::

   Append to the file, or edit it in place. Compose mounts ``switch.txt``
   as a single file, and an editor that saves by writing a new file and
   renaming it leaves the container looking at the old one.

5. See who is connected
-----------------------

The conductor writes its state to a file, and ``vnet-state`` prints it.
``vnet-state`` is not in the container images, so build it once on your
machine (this needs ``cmake``, ``g++``, ``protobuf`` and ``libssl``
development packages):

.. code-block:: console

   $ cmake -S .. -B ../build -DCMAKE_BUILD_TYPE=Release
   $ cmake --build ../build --target vnet-state
   $ docker kill -s USR1 vnet-conductor
   $ docker cp vnet-conductor:/var/vnet-dump.bin .
   $ ../build/vnet-state vnet-dump.bin auth.txt
   dump written 2026-11-11 18:53:32

   switches (3)
     switch3  172.18.0.3:6002  network=::internet  since 2026-11-11 18:52:43
     switch2  172.18.0.4:6001  network=::internet  since 2026-11-11 18:52:43
     switch1  172.18.0.5:6000  network=::internet  since 2026-11-11 18:52:43

   agents (2)
     agent1  10.0.1.1  on switch2  since 2026-11-11 18:52:48
     agent2  10.0.1.2  on switch3  since 2026-11-11 18:52:48

   expected but not connected (0)

With the auth file as second argument, ``vnet-state`` also lists the
machines that should be connected and are not. During an event, that list
is the first thing to look at.

6. Lose a switch
----------------

Stop the switch that holds ``agent2`` (``switch3`` above; use yours):

.. code-block:: console

   $ docker stop vnet-switch3
   $ docker logs --since 10s vnet-agent2
   [Agent] Connection closed: fd=5 (switch)
   [Agent] Switch link lost; joining again
   [Agent] Connection closed: fd=4 (conductor)
   [Agent] Connecting to conductor vnet-conductor:5000 ...
   [Agent] Assigned to switch switch1 @ 172.18.0.5:6000
   [Agent] Authenticated with switch switch1, virtual IP: 10.0.1.2
   [Agent] Entering event loop.
   $ docker exec vnet-agent ping -c 2 10.0.1.2
   2 packets transmitted, 2 received, 0% packet loss, time 1030ms

The agent asked the conductor for another switch and kept its address.
Start the switch again with ``docker start vnet-switch3``.

7. Lose the conductor
---------------------

.. code-block:: console

   $ docker stop vnet-conductor
   $ docker exec vnet-agent ping -c 2 10.0.1.2
   2 packets transmitted, 2 received, 0% packet loss, time 1001ms
   $ docker start vnet-conductor
   $ docker logs --since 10s vnet-conductor 2>&1 | grep -v TLS
   ...
   [Conductor] Previous dump had 3 switch(es) and 2 agent(s)
   [Conductor] Listening on port 5000
   [Conductor] Switch registered: switch3 @ 172.18.0.3:6002 (network=::internet)
   [Conductor] Switch registered: switch1 @ 172.18.0.5:6000 (network=::internet)
   [Conductor] Agent agent2 registered with IP 10.0.1.2 on switch switch1
   ...
   [Conductor] Agent reconnected: agent1 on switch2
   [Conductor] Agent reconnected: agent2 on switch1

Machines kept talking while the conductor was down. When it came back,
the switches registered again and told it which machines they hold, so it
rebuilt its state without any action from you.
:doc:`../concepts/recovery` describes every failure case.

8. Clean up
-----------

.. code-block:: console

   $ docker compose down

Next steps
----------

* :doc:`first-event` prepares a real deployment from a roster.
* :doc:`../concepts/architecture` explains the three programs and how they
  work together.
