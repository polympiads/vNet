Configuration
=============

An event is described by four kinds of files. Each has a reference page;
this page says which program reads which file and how a change reaches the
machines.

.. list-table::
   :header-rows: 1
   :widths: 22 22 56

   * - File
     - Read by
     - Holds
   * - auth file
     - conductor
     - every switch and agent name with its key
       (:doc:`../reference/auth-file`)
   * - switch config
     - conductor, sent to the switches
     - every machine's virtual address and the access list
       (:doc:`../reference/switch-config`)
   * - bypass file
     - one agent
     - what stays on the physical network for that machine
       (:doc:`../reference/bypass-file`)
   * - environment
     - each program
     - addresses, keys, TLS files, switches
       (:doc:`../reference/environment`)

You rarely write them by hand: ``vnet-roster`` generates all of them from
one roster file (:doc:`../reference/roster`).

One access list for the whole network
-------------------------------------

Every switch must apply the same access list, or a pair of machines would
be allowed on one switch and refused on another. There are two ways to get
there.

Pushed by the conductor (recommended)
   Set ``VNET_CONFIG_PATH`` on the conductor. The conductor sends the file
   to every switch as it registers, before any token or route, and again
   after every reload. A switch without a file of its own waits for it and
   refuses agents until it arrives. This is what ``vnet-roster`` sets up.

Read by each switch
   Leave ``VNET_CONFIG_PATH`` unset on the conductor and set it on every
   switch, pointing to identical copies. The conductor compares the
   switches' fingerprints and refuses a switch whose file differs from the
   switches already registered (``VNET_CONFIG_HASH_CHECK=0`` turns the
   check off).

A config pushed by the conductor replaces a switch's own file, whichever
the switch started with.

Fingerprints
------------

The fingerprint of a config is a SHA-256 of its parsed content in a fixed
order, shown as its first 12 hex digits in the logs. Two files that mean
the same have the same fingerprint, however they are ordered or commented.
Every switch reports the fingerprint it applies; the conductor logs a
switch that runs another one.

Changing a file during an event
-------------------------------

.. list-table::
   :header-rows: 1
   :widths: 25 35 40

   * - Change
     - How
     - Takes effect
   * - access list
     - edit the conductor's switch config, then
       ``systemctl reload vnet-conductor``
     - every switch applies it to the next packet; connected machines stay
       connected
   * - a machine's virtual address
     - same
     - when that machine next joins (restart its agent)
   * - a new machine
     - add it to the auth file and the switch config, reload the conductor
     - when its agent starts
   * - a key
     - edit the auth file, reload the conductor
     - at the machine's next join; a machine already joined stays
   * - one machine's bypass file
     - edit it, ``systemctl reload vnet-agent`` on that machine
     - at once: routes and lockdown rules are rebuilt
   * - an environment variable
     - edit the env file, ``systemctl restart`` the program
     - at restart

A reload that fails to parse changes nothing: the previous auth table,
access list or bypass rules stay in force, and the log says which line is
wrong. Check the log after every reload
(:doc:`../guides/during-event`).
