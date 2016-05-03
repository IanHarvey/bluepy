#!/usr/bin/env python
from __future__ import print_function
import argparse
import binascii
import time
import os
import sys
import string

# Add btle.py path for import
sys.path.insert(0,os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'bluepy')))
import btle

if os.getenv('C','1') == '0':
    ANSI_RED = ''
    ANSI_GREEN = ''
    ANSI_YELLOW = ''
    ANSI_CYAN = ''
    ANSI_WHITE = ''
    ANSI_OFF = ''
else:
    ANSI_CSI = "\033["
    ANSI_RED = ANSI_CSI + '31m'
    ANSI_GREEN = ANSI_CSI + '32m'
    ANSI_YELLOW = ANSI_CSI + '33m'
    ANSI_CYAN = ANSI_CSI + '36m'
    ANSI_WHITE = ANSI_CSI + '37m'
    ANSI_OFF = ANSI_CSI + '0m'

def dump_services(dev):
    services = sorted(dev.getServices(), key=lambda s: s.hndStart)
    for s in services:
        print ("\t%02x/%2d: %s" % (s.hndStart, s.hndStart, s))
        if s.hndStart == s.hndEnd:
            continue
        chars = s.getCharacteristics()
        for i, c in enumerate(chars):
            props = c.propertiesToString()
            h = c.getHandle()
            if 'READ' in props:
                try:
                    val = c.read()
                    if c.uuid == btle.AssignedNumbers.device_name:
                        str = '\'' + ANSI_CYAN + val.decode('utf-8') + ANSI_OFF + '\''
                    elif c.uuid == btle.AssignedNumbers.device_information:
                        str = repr(val)
                    elif all(c in string.printable for c in val):
                        str = '\'' + ANSI_CYAN + val + ANSI_OFF + '\''
                    else:
                        str = '<' + ANSI_YELLOW + binascii.b2a_hex(val).decode('utf-8') + ANSI_OFF + '>'
                except btle.BTLEException as e:
                    if e.code == btle.BTLEException.COMM_ERROR:
                        if e.bt_err == 5:
                            # Device sent "authentication failure"
                            str = ANSI_RED + "DENIED" + ANSI_OFF
                        else:
                            # Device sent other error
                            str = ANSI_RED + "BT error:", e.bt_err, ANSI_OFF
                    else:
                        # Probably internal error
                        raise e
            else:
                str = ''
            print ("\t%02x/%2d:    %-59s %-12s %s" % (h, h, c, props, str))

            while True:
                h += 1
                if h > s.hndEnd or (i < len(chars) -1 and h >= chars[i+1].getHandle() - 1):
                    break
                try:
                    val = dev.readCharacteristic(h)
                    print ("\t%02x/%2d:     <%s>" % (h, h, binascii.b2a_hex(val).decode('utf-8')))
                except btle.BTLEException:
                    break

class ScanPrint(btle.DefaultDelegate):
    def handleScan(self, dev, isNewDev, isNewData):
        global matched

        if isNewDev:
            status = "new"
        elif isNewData:
            if arg.new: return
            status = "update"
        else:
            if not arg.all: return
            status = "old"

        # Filter on RSSI
        rssi_val = sum(dev.rssi) / len(dev.rssi)
        if rssi_val < arg.sensitivity:
            return

        # Filter on name or BDADDR
        if arg.filter and arg.filter not in dev.addr:
            for (id, desc, val) in dev.getScanData():
                if id in [8,9] and arg.filter in val:
                    break
            else:
                return

        print ('    Device (%s): %s (%s), %d dBm %s' %
                  (status,
                   ANSI_WHITE + dev.addr + ANSI_OFF,
                   dev.atype,
                   rssi_val,
                   ('' if dev.connectable else '(not connectable)') )
              )
        for (sdid, desc, val) in dev.getScanData():
            if sdid in [8,9]:
                print ('\t' + desc + ': \'' + ANSI_CYAN + val + ANSI_OFF + '\'')
            else:
                print ('\t' + desc + ': <' + val + '>')
        if not dev.scanData:
            print ('\t(no data)')
        print

        # Add device to connect list
        matched += [dev.addr]


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    #parser.add_argument('host', action='store',
    #                    help='BD address of BT device')
    parser.add_argument('-i', '--interface', action='store', default='hci0',
                        help='interface')
    parser.add_argument('-t', '--timeout', action='store', type=int, default=4,
                        help='Scan delay, 0 for continuous')
    parser.add_argument('-s', '--sensitivity', action='store', type=int, default=-128,
                        help='dBm value for filtering far devices')
    parser.add_argument('-f', '--filter', action='store', default=None,
                        help='substring for filtering on name or address')
    parser.add_argument('-d', '--discover', action='store_true',
                        help='Connect and discover service to scanned devices')
    parser.add_argument('-a','--all', action='store_true',
                        help='Display duplicate adv responses, by default show new + updated')
    parser.add_argument('-n','--new', action='store_true',
                        help='Display only new adv responses, by default show new + updated')
    parser.add_argument('-v','--verbose', action='store_true',
                        help='Increase output verbosity')
    arg = parser.parse_args(sys.argv[1:])

    btle.Debugging = arg.verbose

    matched = []
    scanner = btle.Scanner(arg.interface).withDelegate(ScanPrint())

    print (ANSI_RED + "Scanning for devices..." + ANSI_OFF)
    devices = scanner.scan(arg.timeout)

    if arg.discover:
        print (ANSI_RED + "Discovering services..." + ANSI_OFF)

        for d in devices:
            if not d.connectable or d.addr not in matched:
                continue

            print ("    Connecting to", ANSI_WHITE + d.addr + ANSI_OFF + ":")

            try:
                dev = btle.Peripheral(d, iface=arg.interface)
                dump_services(dev)
                dev.disconnect()
            except btle.BTLEException as e:
                print("\t" + ANSI_RED + e.message + ANSI_OFF)
                print("\tConnection failed")
            print





