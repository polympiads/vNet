Manage certificates
===================

With ``VNET_TLS=1`` every link is mutual TLS: each side shows a certificate
signed by the event's CA, and each program checks the role written in the
peer certificate's OU field (``conductor``, ``switch`` or ``agent``).
``tools/vnet-certs.sh``, installed as ``vnet-certs``, makes the CA and the
certificates; ``vnet-roster --tls`` calls it for every machine of the
roster.

Make a CA and certificates
--------------------------

.. code-block:: console

   $ vnet-certs init certs
   wrote certs/ca.pem and certs/ca.key
   $ vnet-certs issue certs conductor conductor
   wrote certs/conductor.pem and certs/conductor.key (OU=conductor)
   $ vnet-certs issue certs switch switch1
   wrote certs/switch1.pem and certs/switch1.key (OU=switch)
   $ vnet-certs issue certs agent c001
   wrote certs/c001.pem and certs/c001.key (OU=agent)

The CA is RSA 3072, machine keys RSA 2048, both valid for 400 days
(``VNET_CERT_DAYS`` changes that). ``init`` refuses to overwrite an
existing CA.

Each machine gets:

.. code-block:: sh

   VNET_TLS=1
   VNET_TLS_CA=/etc/vnet/certs/ca.pem
   VNET_TLS_CERT=/etc/vnet/certs/<name>.pem
   VNET_TLS_KEY=/etc/vnet/certs/<name>.key

``ca.key`` is needed only to issue certificates. Keep it on one organizer
machine, or offline, never on a switch, the conductor or a contestant
machine.

One certificate per machine, or per role
----------------------------------------

The programs check the role, not the name in the certificate. One
certificate per role (one for all agents) works, but then a key copied
from one contestant machine is valid on all of them; with one certificate
per machine, which ``vnet-roster`` makes, a leaked file is one machine's.

Check a certificate
-------------------

.. code-block:: console

   $ openssl x509 -in certs/c001.pem -noout -subject -enddate
   subject=CN = c001, OU = agent, O = vnet
   notAfter=Dec 16 10:21:07 2027 GMT
   $ openssl verify -CAfile certs/ca.pem certs/c001.pem
   certs/c001.pem: OK

Check the dates of every certificate before an event; an expired one
fails the TLS handshake and the machine cannot join.

Add a machine
-------------

Issue its certificate with the same CA; nothing else changes:

.. code-block:: console

   $ vnet-certs issue certs agent c003

``vnet-roster --tls`` does this for machines added to the roster, and
keeps the certificates that already exist.

Replace a certificate
---------------------

Delete the machine's ``.pem`` and ``.key``, issue them again, copy them to
the machine, and restart its program. The certificate is read at start.

Replace the CA
--------------

A new CA means new certificates for every machine, and every program
restarted with them. Do it between events, not during one.

The role check
--------------

``VNET_TLS_ROLE_CHECK=0`` turns the OU check off, so any certificate signed
by the CA may take any role. It exists for certificates made elsewhere
without the OU field; with ``vnet-certs`` leave it on.

When the handshake fails
------------------------

The side that failed logs ``[TLS] handshake failed on fd N`` (a program
that accepts) or ``[TLS] connect`` (a program that dials), followed by
OpenSSL's reason. These were observed with vNet's own programs:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - OpenSSL says
     - Cause
   * - ``certificate verify failed`` (on the side that dials)
     - the other side's certificate is not signed by this side's CA, or has
       expired
   * - ``tlsv1 alert unknown ca`` (on the side that accepts)
     - the side that dialed did not accept this side's certificate: the
       two machines use different CAs
   * - ``wrong version number``
     - the other side runs without TLS (``VNET_TLS`` unset, with
       ``VNET_ALLOW_INSECURE=1``)
   * - ``unexpected eof while reading``
     - the peer closed the connection during the handshake; usually a port
       check or a health check, and harmless

A certificate with the wrong role is accepted by TLS and refused right
after, with ``certificate role is '<role>'`` in the log of the side that
checked it.
