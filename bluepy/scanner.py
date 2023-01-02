#!/usr/bin/python

from __future__ import print_function

import sys
from time import gmtime, strftime

from bluepy.btle import Scanner, DefaultDelegate


class ScanDelegate(DefaultDelegate):
    def handleDiscovery(self, dev, isNewDev, isNewData):
        print(strftime("%Y-%m-%d %H:%M:%S", gmtime()), dev.addr, dev.getScanData())
        sys.stdout.flush()


scanner = Scanner().withDelegate(ScanDelegate())

# listen for ADV_IND packages for 10s, then exit
scanner.scan(10.0, passive=True)
