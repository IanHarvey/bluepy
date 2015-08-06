The ``UUID`` class
==================

Constructor
-----------

.. function:: UUID(value)

    Constructs a ``UUID`` object with the given *value*. This may be:

    * an `int` value in the range 0 to 0xFFFFFFFF
    * a `str` value 
    * another `UUID` object
    * any other value which can be converted to hex digits using ``str()``

Instance Methods
----------------

.. function:: getCommonName()

    Returns string describing that UUID. If the UUID is one listed
    in :ref:`assignednumbers` this will be a human-readable name e.g.
    "Cycling Speed and Cadence". Otherwise, it will be a hexadecimal string.

