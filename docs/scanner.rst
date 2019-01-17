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

.. function:: Scanner( [iface=0] )

    Creates and initialises a new scanner object. *iface* identifies the
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
    *process()* and *stop()* methods in order.
     
.. function:: clear()

    Clears the current set of discovered devices. 
    
.. function:: start()

    Enables reception of advertising broadcasts from peripherals.
    Should be called before calling *process()*.

.. function:: process ( [timeout = 10] )

    Waits for advertising broadcasts and calls the *delegate* object
    when they are received. Returns after the given *timeout* period
    in seconds. This may be called multiple times (between calls to
    *start()* and *stop()* ).
    
.. function:: stop()

    Disables reception of advertising broadcasts. Should be called after
    *process()* has returned.

.. function:: getDevices()

    Returns a list (a *view* on Python 3.x) of ``ScanEntry`` objects for
    all devices which have been discovered (since the last *clear()* call).

Sample code
-----------

Basic code to run a LE device scan for 10 seconds follows this example::
    
    # import the necessary parts of the bluepy library
    from bluepy.btle import Scanner, DefaultDelegate

    # create a delegate class to receive the BLE broadcast packets 
    class ScanDelegate(DefaultDelegate):
        def __init__(self):
            DefaultDelegate.__init__(self)

        # when this python script discovers a BLE broadcast packet, print a message with the device's MAC address
        def handleDiscovery(self, dev, isNewDev, isNewData):
            if isNewDev:
                print "Discovered device", dev.addr
            elif isNewData:
                print "Received new data from", dev.addr

    # create a scanner object that sends BLE broadcast packets to the ScanDelegate
    scanner = Scanner().withDelegate(ScanDelegate())
    
    # create a list of unique devices that the scanner discovered during a 10-second scan
    devices = scanner.scan(10.0)

    # for each device  in the list of devices 
    for dev in devices:
        # print  the device's MAC address, its address type, 
        # and Received Signal Strength Indication that shows how strong the signal was when the script received the broadcast.   
        print "Device %s (%s), RSSI=%d dB" % (dev.addr, dev.addrType, dev.rssi)

        # For each of the device's advertising data items, print a description of the data type and value of the data itself 
        # getScanData returns a list of tupples: adtype, desc, value
        # where AD Type means “advertising data type,” as defined by Bluetooth convention:
        # https://www.bluetooth.com/specifications/assigned-numbers/generic-access-profile 
        # desc is a human-readable description of the data type and value is the data itself  
        for (adtype, desc, value) in dev.getScanData():
            print "  %s = %s" % (desc, value)


For continuous scanning, follow this example::
    
    # import the necessary parts of the bluepy library
    from bluepy.btle import Scanner, DefaultDelegate

    # create a delegate class to receive the BLE broadcast packets 
    class ScanDelegate(DefaultDelegate):
        def __init__(self):
            DefaultDelegate.__init__(self)

        # when this python script discovers a BLE broadcast packet, print a message with the device's MAC address
        def handleDiscovery(self, dev, isNewDev, isNewData):
            if isNewDev:
                print "Discovered device", dev.addr
            elif isNewData:
                print "Received new data from", dev.addr

    # create a scanner object that sends BLE broadcast packets to the ScanDelegate
    scanner = Scanner().withDelegate(ScanDelegate())
    
    # start the scanner and keep the process running 
    scanner.start()
    while True:
        print "Still running..."
        scanner.process()


NOTE that (1) LE scanning must be run as root and (2) the continuous scanning code never completes, so you'll need to do useful things in the `handleDiscovery()` method of your delegate.  

See the documentation for ``ScanEntry`` for the information available via the *dev*
parameter passed to the delegate.

