.. _service:

The ``Service`` class
=====================

A Bluetooth LE ``Service`` object represents a collection of `characteristics` and
`descriptors` which are all related to one particular function of the peripheral. This
allows particular characteristics to be discovered without having to enumerate everything offered by that peripheral.
 
More information about standard services can be found at
https://www.bluetooth.org/en-us/specification/adopted-specifications

Constructor
-----------

You should not construct ``Service`` objects directly. Instead, use the
``getServices()`` or ``getServiceByUUID()`` methods of a connected ``Peripheral`` object.
  
 
Instance Methods
----------------

.. function:: getCharacteristics([forUUID=None])

    Returns a list of ``Characteristic`` objects associated with the service. If this 
    has not been done before, the peripheral is queried. Otherwise, a cached list if
    returned. If *forUUID* is given, it may be a ``UUID`` object or a value used to 
    construct one.  In this case the returned list, which may be empty, contains any
    characteristics associated with the service which match that UUID.
