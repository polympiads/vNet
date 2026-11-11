Your first event
================

This tutorial takes a small event from nothing to running: one conductor,
two switches, one judge machine and two contestant machines. A larger
event is the same with more lines in the roster. Allow an hour the first
time.

The plan
--------

.. list-table::
   :header-rows: 1
   :widths: 20 25 55

   * - Machine
     - Address
     - Role
   * - ``conductor``
     - ``192.0.2.5``
     - Admits machines and holds the rules. Organizer machine.
   * - ``switch1``, ``switch2``
     - ``192.0.2.11``, ``192.0.2.12``
     - Check and forward packets. Organizer machines.
   * - ``judge``
     - virtual ``10.0.0.1``
     - Runs the contest server. Reaches the internet.
   * - ``c001``, ``c002``
     - virtual ``10.0.1.1``, ``10.0.1.2``
     - Contestant machines. Reach the judge and nothing else.

Network requirements, on the physical network:

* every switch and every agent reaches the conductor on TCP port 5000;
* every agent reaches every switch on its port (6000, 6001, ...);
* switches reach each other on those ports;
* switches reach the internet, if any machine is allowed to.

Organizer machines must not have an address inside the overlay prefix,
``10.0.0.0/8`` by default.

1. Install vNet
---------------

On each organizer machine and in the image of the contestant machines,
build and install. The dependencies are those of a C++17 project with
protobuf and OpenSSL; on Ubuntu:

.. code-block:: console

   $ sudo apt-get install -y g++ cmake libprotobuf-dev protobuf-compiler libssl-dev iproute2 iptables
   $ git clone https://github.com/polympiads/vNet.git && cd vNet
   $ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   $ cmake --build build -j
   $ sudo cmake --install build

This puts ``conductor``, ``switch``, ``agent``, ``vnet-state``,
``vnet-certs`` and ``vnet-roster`` in ``/usr/local/bin`` and the three
systemd units in ``/usr/local/lib/systemd/system``.

2. Write the roster
-------------------

On your own machine (not on the event's machines), write ``roster.txt``:

.. code-block:: text

   switch switch1
   switch switch2
   agent  judge 10.0.0.1 judges internet=0.0.0.0/0
   agent  c001  10.0.1.1 contestants lockdown
   agent  c002  10.0.1.2 contestants lockdown
   allow  contestants judges

Each ``agent`` line gives a machine its virtual address and its groups;
``allow`` lets two groups talk; ``internet=`` lists the internet addresses a
machine may reach; ``lockdown`` closes the physical network around the
overlay on that machine. :doc:`../reference/roster` describes every
option.

3. Generate the files
---------------------

.. code-block:: console

   $ vnet-roster roster.txt out --conductor 192.0.2.5 --tls
   wrote out/certs/ca.pem and out/certs/ca.key
   wrote out/certs/conductor.pem and out/certs/conductor.key (OU=conductor)
   ...
   wrote out: 2 switch(es), 3 agent(s), 2 allowed pair(s), 0 bypass file(s), certificates

``out/`` now holds everything:

.. code-block:: text

   out/auth.txt               the machines and their keys, for the conductor
   out/switch.txt             the access list, for the conductor
   out/machines/<name>.env    the environment of each machine's service
   out/certs/                 the CA and one certificate per machine

Keep ``out/`` private: it holds every key. ``out/certs/ca.key`` stays on
your machine and goes nowhere else.

4. Put the files on the machines
--------------------------------

Every machine reads its settings from ``/etc/vnet``:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Machine
     - Files
   * - conductor
     - ``out/auth.txt`` as ``/etc/vnet/auth.txt``; ``out/switch.txt`` as
       ``/etc/vnet/switch.txt``; ``out/machines/conductor.env`` as
       ``/etc/vnet/conductor.env``; ``ca.pem``, ``conductor.pem`` and
       ``conductor.key`` into ``/etc/vnet/certs/``
   * - each switch
     - ``out/machines/<name>.env`` as ``/etc/vnet/switch.env``; ``ca.pem``,
       ``<name>.pem`` and ``<name>.key`` into ``/etc/vnet/certs/``
   * - each agent
     - ``out/machines/<name>.env`` as ``/etc/vnet/agent.env``; its bypass
       file, if it has one, as ``/etc/vnet/bypass.conf``; ``ca.pem``,
       ``<name>.pem`` and ``<name>.key`` into ``/etc/vnet/certs/``

Make the key files readable by root only (``chmod 600``).

A switch turns on IPv4 forwarding and loose reverse-path filtering itself
when it starts (``net.ipv4.ip_forward=1``, ``rp_filter=0``) and adds its
NAT rule; there is nothing to do unless something on the machine resets
them.

5. Start the services
---------------------

Start the conductor first; the order does not matter otherwise, since
switches and agents retry every two seconds.

.. code-block:: console

   conductor$ sudo systemctl enable --now vnet-conductor
   switch1$   sudo systemctl enable --now vnet-switch
   c001$      sudo systemctl enable --now vnet-agent

6. Check
--------

On the conductor:

.. code-block:: console

   $ sudo systemctl kill -s USR1 vnet-conductor
   $ sudo vnet-state /var/vnet-dump.bin /etc/vnet/auth.txt

Wait until the list *expected but not connected* is empty. Then, from a
contestant machine:

* ``ping 10.0.0.1`` answers: contestants may reach the judge;
* ``ping 10.0.1.2`` from ``c001`` does not: contestants may not reach each
  other;
* the LAN gateway does not answer: lockdown closed the physical network.

Repeat the first ping from contestants on each switch, so that a path
across two switches is tested.

Next steps
----------

* :doc:`../guides/prepare-event` is the checklist for a real event.
* :doc:`../guides/vdi-lockdown` if the contestant machines are VDI
  desktops.
* :doc:`../guides/during-event` for changes once the contest has started.
