bluepy
======

Python interface to Bluetooth LE on Linux

This is a project to provide an API to allow access to Bluetooth Low Energy devices
from Python. At present it runs on Linux only; I've mostly developed it using a
Raspberry Pi, but it will also run on x86 Debian Linux.

The code is tested on Python 2.7 and 3.4; it should also work on 3.3.

There is also code which uses this to talk to a TI SensorTag (www.ti.com/sensortag).

An example to interface the Nordic Semiconductor ASA IoT Sensor Kit, Thingy:52 is available 
in thingy52.py (https://www.nordicsemi.com/eng/Products/Nordic-Thingy-52).

Installation
------------

The code needs an executable `bluepy-helper` to be compiled from C source. This is done
automatically if you use the recommended pip installation method (see below). Otherwise,
you can rebuild it using the Makefile in the `bluepy` directory.

To install the current released version, on most Debian-based systems:

    $ sudo apt-get install python-pip libglib2.0-dev
    $ sudo pip install bluepy

On Fedora do:

    $ sudo dnf install python-pip glib2-devel

For Python 3, you may need to use `pip3`:

    $ sudo apt-get install python3-pip libglib2.0-dev
    $ sudo pip3 install bluepy

*If this fails* you should install from source.

    $ sudo apt-get install git build-essential libglib2.0-dev
    $ git clone https://github.com/IanHarvey/bluepy.git
    $ cd bluepy
    $ python setup.py build
    $ sudo python setup.py install

I would recommend having command-line tools from BlueZ available for debugging. There
are instructions for building BlueZ on the Raspberry Pi at http://www.elinux.org/RPi_Bluetooth_LE.

Documentation
-------------

Documentation can be built from the sources in the docs/ directory using Sphinx.

An online version of this is currently available at: http://ianharvey.github.io/bluepy-doc/

License
-------

This project uses code from the bluez project, which is available under the Version 2
of the GNU Public License.

The Python files are released into the public domain by their author, Ian Harvey.

Release Notes
-------------

Release 1.3.0

- New getState() method for Peripheral class
- New exception structure / error reporting (#311, #317, #326)
  BTLEException now has subclasses BLTEDisconnectError, BTLEManagementError, etc.
  which report an error code and error message passed up from the lower layers, where
  appropriate.
- Partial merge #311: aids to debugging; bluepy-helper reports version; fix crash
- Partial merge #311 and #302: pair() and unpair() now supported
- Fix #169: 0-byte characteristic writes are now supported
- Merge #302: OOB data now supported
- Merge #312: better comments on sample code in docs, better scanner example
- Fix #292: Unicode string decoding errors in scan data
- Merge #308: don't ignore sensitivity option during discovery
- Merge #301: fix Peripheral documentation
- Fix #286: return list of services from Scan entry


Release 1.2.0
- Merge #245: Update underlying Bluez version to 5.47
- Merge #284: Readme updated with Fedora install instructions
- Merge #283: Fixes for passive scan interruption
- Merge #275, fix #259: non-ASCII device names now don't break decoding
- Fix #263, #278: return UUID in scan results
- Merge #262: Return correct address type in passive scan
 

Release 1.1.4:
- Further attempts to fix #158. setup.py rewritten.

*There was no release 1.1.3 made*

Release 1.1.2: *now deleted*
- Re #158: Try to make PyPI installation more robust
- Merge #214: add passive scan support
- Merge #213: Add Thingy:52 support

Release 1.1.1
- Workaround #200: remove -Werror from Makefile
- Fix #191: generate BTLEException not ValueError, if helper is killed
- Fix #189: error calling getCharacteristics() when Service has no characteristics
- Workaround #192: Use make -j1 explicitly

Release 1.1.0
- Merge #180: Peripheral.connect() can now take ScanEntry object (like constructor)
- Merge #162: Add build_ext builder to setup.py
- Merge #166: Fix crash in getServiceByUUID()
- Fix #148: Add UUIDs for declarations (e.g. 0x2800 = Primary Service Declaration)
- Fix #28: Sensortag accelerometer values now scaled properly
- Merge #89: Add support for descriptors
- Fix #157: make 'services' a property
- Fix #111: make parameter names match documentation
- Fix #128: Characteristic.write() was missing a return value
- Read battery level on Sensortag
- Formatting/style fixes (#170 and others)

Release 1.0.5
- Fix issue #123: Scanner documentation updated
- Fix #125: setup.py error reporting on Python 3 if compilation fails
- Fix for issue #127: setup.py fails to rebuild bluepy-helper 

Release 1.0.4
- Scanner now available as bluepy.blescan module and 'blescan' command
- Fix example scanner code in documentation
- Python 3 installation fixes
- Fix issues #69, #112, #115, #119

Release 1.0.3
- Now available on PyPI as `bluepy`. Installs via pip.

Release 0.9.12
- Support for CC2650 sensortag
- Documentation fixes
- Bug fix: DefaultDelegate has a handleDiscovery method
- Bug fix: keypress now works with both V1.4 and V1.5 firmware 


Release 0.9.11

- Minor consistency improvements & bug fixes
- Scanner now has getDevices() call
- Docs updated

Release 0.9.10

- Now with Scan functionality

Release 0.9.9

- Now based on Bluez r5.29
- UUIDs held in separate JSON file, script added to update from Web
- Added setup.py and __init__.py for use with setuptools
- Allows indications as well as notifications
- Bug fixes (see pull requests #46, #48, #35)

Release 0.9.0
- Support for Notifications
- SensorTag code now supports keypress service
- Bug fix for SetSecurityLevel
- Support for Random address type
- More characteristic and service UUIDs added

Release 0.2.0

- Sphinx-based documentation
- SensorTag optimisations 
- Improved command line interface to sensortag.py
- Added .gitignore file (github issue #17)

Release 0.1.0
- this has received limited testing and bug fixes on Python 3.4.1
- fix for exceptions thrown if peripheral sends notifications

Release dated 2-Jul-2014

- expand AssignedNumbers class definitions
- add getCommonName() to UUID type, returns human-friendly string

Release dated 14-Apr-2014:

- make btle.py useful from the command line
- add AssignedNumbers class

Release dated 12-Mar-2014
- add exceptions, and clean up better on failure

Initial release 19-Oct-2013:

TO DO list
----------

The following are still missing from the current release:
- Unit test 
- Peripheral role support



