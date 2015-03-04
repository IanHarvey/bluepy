.. _characteristic:

The ``Characteristic`` class
============================

A Bluetooth LE "characteristic" represents a short data item which can be read or
written. These can be fixed (e.g. a string representing the manufacturer name) or
change dynamically (such as the current temperature or state of a button). Most
interaction with Bluetooth LE peripherals is done by reading or writing characteristics. 

Constructor
-----------

You should not construct ``Characteristic`` objects directly. Instead, use the
``getCharacteristics()`` method of a connected ``Peripheral`` object.

Instance Methods
----------------

.. function:: read()

    Reads the current value of a characteristic as a string of bytes. For Python 2.x this
    is a value of type `str`, and on Python 3.x this is of type `bytes`. This may be
    used with the `struct` module to extract integer values from the data. 
    

.. function:: write(data, [withResponse=False])

    Writes the given *data* to the characteristic. *data* should be of type `str` for
    Python 2.x, and type `bytes` for Python 3.x. Bluetooth LE allows the sender to
    request the peripheral to send a response to confirm that the data has been received.
    Setting the *withResponse* parameter to *True* will make this request. A 
    `BTLEException` will be raised if the confirmation process fails.
    
.. function:: supportsRead()

    Returns *True* if the characteristic can be read (as indicated by its properties)
    and *False* otherwise.
 
.. function:: propertiesToString()

    Returns a string describing the characteristic properties ('READ', 'WRITE', etc).

.. function:: getHandle()

    Returns the 16-bit integer value used to identify the characteristic in the
    underlying GATT protocol. This may be useful to distinguish between notifications
    from different characteristics (see :ref:`notifications` for further information).


