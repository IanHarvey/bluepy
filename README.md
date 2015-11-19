bluepy
======

Python interface to Bluetooth LE on Linux

This is a project to provide an API to allow access to Bluetooth Low Energy devices
from Python. At present it runs on Linux only; I've mostly developed it using a
Raspberry Pi, but it will also run on x86 Debian Linux.

There is also code which uses this to talk to a TI SensorTag  and Ti SensorTag 2 (www.ti.com/sensortag).

Installation
------------

The code needs an executable 'bluepy-helper' to be compiled from C source. Currently the Makefile is configured to build for ARM Linux; you will need to set the ARCH variable in it  appropriately for other platforms. The sources need glib and dbus headers to compile.

There are general instructions for setting up BlueZ on the Raspberry Pi at http://www.elinux.org/RPi_Bluetooth_LE.

To build on the Pi:

    $ sudo apt-get install build-essential libglib2.0-dev libdbus-1-dev
    $ git clone https://github.com/IanHarvey/bluepy.git
    $ cd bluepy
    $ make

Once 'bluepy-helper' is built, you can copy it and the two .py files to somewhere
convenient on your Python path (e.g. /usr/local/lib/python2.7/site-packages/).

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
- Build into easily installable package
- Implement 'hcitool lescan' functionality
- Reading RSSI
- Unit test 


