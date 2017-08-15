#!/usr/bin/python
from __future__ import print_function

from time import gmtime, strftime, sleep
from bluepy.btle import Scanner, DefaultDelegate, BTLEException
import sys


class ScanDelegate(DefaultDelegate):

    def handleDiscovery(self, dev, isNewDev, isNewData):
        print(strftime("%Y-%m-%d %H:%M:%S", gmtime()), dev.addr, dev.getScanData())
        sys.stdout.flush()

scanner = Scanner().withDelegate(ScanDelegate())

# listen for ADV_IND packages for 10s, then exit
scanner.scan(10.0, passive=True)
