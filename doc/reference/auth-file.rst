Auth file
=========

The auth file lists every machine allowed to join, with its key. Only the
conductor reads it (``VNET_AUTH_FILE``); ``vnet-roster`` writes it as
``auth.txt``. Keep it readable by root only.

Format
------

One machine per line:

.. code-block:: text

   switch <name> <key>
   agent  <name> <key>

Blank lines and lines whose first non-blank character is ``#`` are
ignored. A key is any word without spaces; ``vnet-roster`` makes 32 hex
digits. Agent names are 1 to 10 characters, switch names 1 to 32, from
``[A-Za-z0-9_-]``. A name appears once, whatever its role.

The machine gives the same key in ``VNET_AUTH_KEY`` or
``VNET_AUTH_KEY_FILE``. The conductor accepts a join when the name is in
the file with the role the machine claims and the key matches.

.. code-block:: text

   # organizers
   switch switch1 9e074da4ff78cddb1e911c1a8ea34867
   switch switch2 3b1c7f0e5a9d4c2b8e6f1a0d9c8b7a65
   # machines
   agent  judge   0efe8fa9cc0bc6043b2e20aecab94306
   agent  c001    49ac8b08c1914e48ee6b2a02a8b1d041

Reload
------

``systemctl reload vnet-conductor`` (``SIGHUP``) reads the file again. A
machine added can join at once; a machine removed is refused at its next
join, and one already connected stays until then. A file with an error
changes nothing:

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Message
     - Cause
   * - ``Auth file line N: expected 'switch <name> <key>' or 'agent <name> <key>'``
     - a line with fewer than three words, or another first word
   * - ``Auth file line N: invalid agent name '...'``
     - a name too long or with other characters
   * - ``Auth file line N: <name> is listed twice``
     - the same name on two lines
   * - ``Auth file <path> names no machine``
     - an empty file
   * - ``Cannot open auth file <path>``
     - wrong path or permissions

After a successful reload: ``[Conductor] Auth file reloaded: N
record(s)``.

``vnet-state`` takes the auth file as its second argument to list the
machines it names that are not connected.
