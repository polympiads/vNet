Overview
========

vNet gives every machine of an on-site programming contest exactly the
network access the rules allow, and nothing else, on infrastructure the
organizers do not control: university rooms, a VDI pool, a shared LAN.

Each machine runs a small program, the **agent**, which opens a virtual
interface and sends all of the machine's IPv4 traffic through the overlay.
**Switches** run by the organizers check every packet against one access
list before forwarding it. A **conductor** admits machines, places them on
switches and distributes the routes and the access list. Decisions are
taken on organizer machines only; a contestant machine only carries
packets.

What vNet gives an event
------------------------

* **One access list for the whole event.** Which machine may talk to
  which, and which internet addresses each may reach, is one text file on
  the conductor. Changing it is one edit and one reload; nothing restarts.
* **Enforcement off the contestant machine.** Every packet is checked on a
  switch for its source address, its destination and its rate. Root on a
  contestant machine does not open the network.
* **Mutual TLS on every link**, with the role of each machine (conductor,
  switch or agent) in its certificate.
* **A host firewall** on contestant machines (*lockdown*) that closes the
  physical network around the overlay, with a bypass file for what must
  stay reachable, such as a VDI broker.
* **Recovery without intervention.** A switch that dies hands its machines
  to the others; a conductor that restarts learns the state back from the
  switches; the data plane keeps running while the conductor is down.
* **One roster for everything.** ``vnet-roster`` turns one file listing the
  machines and the rules into every key, certificate, config and
  environment file the event needs.

Where to start
--------------

* To see vNet run in five minutes on one machine:
  :doc:`tutorials/docker`.
* To prepare a real event: :doc:`tutorials/first-event`, then
  :doc:`guides/prepare-event`.
* During an event, when something is wrong: :doc:`troubleshooting`.
* To understand how it works: :doc:`concepts/architecture`.
* Every variable, file format, command and log line:
  the reference pages, starting with :doc:`reference/environment`.

The source code is at https://github.com/polympiads/vNet, under the MIT
license. Measurements are at
https://github.com/polympiads/vNet-benchmarks.
