.. _delegate:

The ``DefaultDelegate`` class
=============================
  
``bluepy`` functions which receive Bluetooth messages asynchronously -
such as notifications, indications, and advertising data - pass this information
to the user by calling methods on a 'delegate' object.

To be useful, the delegate object will be from a class created by the user.
Bluepy's ``DefaultDelegate`` is a base class for this - you should override
some or all of the methods here with your own application-specific code.

Constructor
-----------

.. function:: DefaultDelegate()

    Initialises the object instance. 


Instance Methods
----------------

.. py:method:: handleNotification(cHandle, data)

   Called when a notification or indication is received from a connected
   ``Peripheral`` object. *cHandle* is the (integer) handle for the 
   characteristic - this can be used to distinguish between notifications
   from multiple sources on the same peripheral. *data* is the characteristic
   data (a ``str`` type on Python 2.x, and ``bytes`` on 3.x).

.. py:method:: handleDiscovery(scanEntry, isNewDev, isNewData)

   Called when advertising data is received from an LE device while a
   ``Scanner`` object is active. *scanEntry* contains device information
   and advertising data - see the ``ScanEntry`` class for details. *isNewDev* 
   is ``True`` if the device (as identified by its MAC address) has not been
   seen before by the scanner, and ``False`` otherwise. *isNewData* is ``True``
   if new or updated advertising data is available.
