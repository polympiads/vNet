.. vNet auth documentation master file, created by
   sphinx-quickstart on Mon Sep 30 11:58:09 2024.
   You can adapt this file completely to your liking, but it should at least
   contain the root `toctree` directive.

vNet : Virtual Network
======================

The objective of this project is to connect virtual machines together in our competitive programming
contests, using TUN interfaces to intercept all the outgoing ip packets and redirect them through our
own IP tunnels.

Where to start
--------------

* :doc:`overview`: what vNet does for an event, in one page.
* :doc:`tutorials/docker`: a whole network on one computer in ten minutes.
* :doc:`tutorials/first-event`: from a roster to running machines.
* :doc:`troubleshooting`: during an event, from the symptom to the fix.
* :doc:`reference/environment`: every setting of every program.

.. toctree::
   :maxdepth: 10
   :hidden:
   :caption: Start here

   overview
   tutorials/docker
   tutorials/first-event

.. toctree::
   :hidden:
   :caption: Guides

   guides/prepare-event
   guides/during-event
   guides/vdi-lockdown
   guides/monitoring
   guides/certificates
   troubleshooting

.. toctree::
   :hidden:
   :caption: Concepts

   concepts/architecture
   concepts/joining
   concepts/data-plane
   concepts/security
   concepts/configuration
   concepts/recovery

.. toctree::
   :hidden:
   :caption: Reference

   reference/environment
   reference/switch-config
   reference/auth-file
   reference/bypass-file
   reference/roster
   reference/commands
   reference/logs
   reference/limits
   reference/protocol

.. toctree::
   :hidden:
   :caption: For developers

   api/index
