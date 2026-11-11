Switch config
=============

The switch config gives every machine its virtual address and says who may
talk to whom. The conductor reads it from ``VNET_CONFIG_PATH`` and sends it
to every switch (:doc:`../concepts/configuration`). ``vnet-roster`` writes
it as ``switch.txt``.

Format
------

One entry per line, two words separated by spaces or tabs. Blank lines and
lines whose first non-blank character is ``#`` are ignored; anything after
the second word is ignored. Windows line endings are accepted. Entries may
come in any order.

``<agent> <a.b.c.d>``
   Gives the agent this virtual address. Each agent and each address
   appears once.

``<agent> <agent>``
   The two machines may talk to each other, in both directions, with any
   protocol and port.

``<agent> ::internet:<a.b.c.d>`` or ``<agent> ::internet:<a.b.c.d/n>``
   The machine may reach this internet address, or every address in the
   prefix, and receive the replies. ``::internet:0.0.0.0/0`` is the whole
   internet.

Agent names are 1 to 10 characters from ``[A-Za-z0-9_-]``. A rule may only
name agents that have an address. Give every address inside the overlay
prefix (``10.0.0.0/8`` unless ``VNET_OVERLAY_CIDR`` says otherwise).

Example
-------

.. code-block:: text

   # virtual addresses
   judge   10.0.0.1
   c001    10.0.1.1
   c002    10.0.1.2

   # contestants reach the judge, not each other
   c001    judge
   c002    judge

   # the judge reaches the internet; c001 one address
   judge   ::internet:0.0.0.0/0
   c001    ::internet:192.0.2.80

Errors
------

A file with an error is refused as a whole; at start the program stops, on
a reload the previous config stays in force. The log names the line:

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Message
     - Cause
   * - ``line N: expected two tokens, got '...'``
     - a line with one word
   * - ``line N: invalid IPv4 '...'``
     - ``0.0.0.0`` as a virtual address
   * - ``line N: agent name '...' must be 1 to 10 characters from [A-Za-z0-9_-]``
     - a name too long or with other characters
   * - ``line N: duplicate IP a.b.c.d``
     - two machines with one address
   * - ``line N: duplicate agent name '...'``
     - one machine with two addresses
   * - ``line N: unknown agent '...'``
     - a rule names a machine that has no address line, or a misspelled
       address (``10.0.1.300`` is read as a name)
   * - ``line N: invalid internet address in '...'``
     - a malformed ``::internet:`` address or prefix

When it parses, the program logs ``[Config] Loaded N agent(s), M with agent
ACLs, K with internet ACLs``.

Fingerprint
-----------

The fingerprint is a SHA-256 of the parsed entries in a fixed order:
comments, blank lines, order and the direction in which a pair is written
do not change it. The logs show its first 12 hex digits.
