.. _scanentry:

The ``ScanEntry`` class
=======================

A ``ScanEntry`` object contains information received from a Bluetooth LE
device received during ``Scanner`` operation. This includes parameters
needed to connect to the device (MAC address, address type), and 
advertising data (such as its name or available services) supplied in
the device's broadcasts.

Constructor
-----------

``ScanEntry`` objects are created by the ``Scanner`` class, and should not
be created by the user.
 
Instance Methods
----------------

.. py:method:: getDescription(adtype)

    Returns a human-readable description of the advertising data 'type'
    code *adtype*. For instance, an *adtype* value of 9 would return the
    string ``"Complete Local Name"``. See the Generic Access Profile 
    assigned numbers at https://www.bluetooth.org/en-us/specification/assigned-numbers/generic-access-profile
    for a complete list.
    
.. py:method:: getValueText(adtype)

    Returns a human-readable string representation of the advertising data
    for code *adtype*. Values such as the 'local name' are returned as
    strings directly; other values are converted to hex strings. If the
    requested data is not available, ``None`` is returned.

.. py:method:: getScanData()

    Returns a list of tuples *(adtype, description, value)* containing the
    AD type code, human-readable description and value (as reported by
    ``getDescription()`` and ``getValueText()``) for all available advertising
    data items.
    
Properties
----------

All the properties listed below are read-only.

.. py:attribute:: addr

    Device MAC address (as a hex string separated by colons).
    
.. py:attribute:: addrType

    Device address type - one of *ADDR_TYPE_PUBLIC* or *ADDR_TYPE_RANDOM*.

.. py:attribute:: iface

    Bluetooth interface number (0 = ``/dev/hci0``) on which advertising information was seen.

.. py:attribute:: rssi

    Received Signal Strength Indication for the last received broadcast from the
    device. This is an integer value measured in dB, where 0 dB is the maximum 
    (theoretical) signal strength, and more negative numbers indicate a weaker signal.

.. py:attribute:: connectable

    Boolean value - ``True`` if the device supports connections, and ``False`` 
    otherwise (typically used for advertising 'beacons').
    
.. py:attribute:: updateCount

    Integer count of the number of advertising packets received from the device
    so far (since *clear()* was called on the ``Scanner`` object which found it).

    
