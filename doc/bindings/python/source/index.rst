########
Welcome!
########

Welcome to `Babeltrace <http://www.efficios.com/babeltrace>`_'s
Python binding's documentation!

Babeltrace is a trace format converter. It is able to read and write
different trace formats, one of them being
`CTF <http://www.efficios.com/ctf>`_ (the Common Trace Format), as
Babeltrace also acts as the reference reader/writer implementation
for this format.

The Babeltrace Python binding sits on top of ``libbabeltrace``, the
current public C API of Babeltrace.


Installing
----------

The Python binding may be enabled when configuring Babeltrace's build::

    ./configure --enable-python-bindings

On Debian and Ubuntu, it is available in the ``python3-babeltrace``
package.

.. note::

   Currently, the Babeltrace Python binding only works with Python 3.


Binding
-------

The Babeltrace Python binding is available as a single Python package,
:py:mod:`babeltrace`.

The Babeltrace Python binding's application programming interface is
divided into two parts:

* The :ref:`reader API <reader-api>` is exposed by the
  :mod:`babeltrace.reader` module, a set of classes used to
  open a collection of different traces an iterate their events.
* The :ref:`CTF writer API <ctf-writer-api>` is exposed by the
  :mod:`babeltrace.writer` module, which makes it possible to
  write a complete `CTF <http://www.efficios.com/ctf>`_
  (Common Trace Format) trace from scratch.

Both modules make use of :mod:`babeltrace.common`, which contains
various :ref:`constants <constants>` used by both the reader and CTF
writer sides.

.. note::

   For backward compatibility reasons, the reader API is imported in the
   package itself. The CTF writer API is imported in the package itself
   too, as :py:mod:`~babeltrace.CTFWriter`. It is, however, strongly
   recommended to import and use the three modules above explicitly, since
   there is no long-term plan to maintain the compatibility layer.

**Contents**:

.. toctree::
   :numbered:

   constants
   reader
   writer
   examples
