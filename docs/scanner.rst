.. _scanner:

The ``Scanner`` class
=====================
  
A ``Scanner`` object is used to scan for LE devices which are broadcasting
advertising data. In most situations this will give a set of devices which
are available for connection. (Note, however, that Bluetooth LE devices may
accept connections without broadcasting advertising data, or may broadcast
advertising data but may not accept connections).


Constructor
-----------

.. function:: Scanner( [index=0] )

    Creates and initialises a new scanner object. *index* identifies the
    Bluetooth interface to use (where 0 is **/dev/hci0** etc). Scanning
    does not start until the *start()* or *scan()* methods are called -
    see below for details.
 
Instance Methods
----------------

.. function:: withDelegate(delegate)

    Stores a reference to a *delegate* object, which receives callbacks
    when broadcasts from devices are received. See the documentation for
    ``DefaultDelegate`` for details. 

.. function:: scan( [timeout = 10] )

    Scans for devices for the given *timeout* in seconds. During this 
    period, callbacks to the *delegate* object will be called. When the
    timeout ends, scanning will stop and the method will return a list
    (or a *view* on Python 3.x) of ``ScanEntry`` objects for all devices
    discovered during that time.
    
    *scan()* is equivalent to calling the *clear()*, *start()*, 
    *process()* and *stop()* methods.
     
.. function:: clear()

    Clears the current set of discovered devices. 
    
.. function:: start()

.. function:: process ( [timeout = 10] )

.. function:: stop()


