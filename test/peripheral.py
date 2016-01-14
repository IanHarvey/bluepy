#!/usr/bin/env python
import argparse
import binascii
import time
import os
import sys
# Add btle.py path for import
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'bluepy')))
import btle
import btle_gatts
from types import MethodType

def cccd_write(self, value):
    print("Writing to BAS CCCD")

def read_not_supported(self):
    raise btle_gatts.AttError(btle_gatts.ATT_ECODE_READ_NOT_PERM, self.h)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-i', '--interface', action='store', default='hci0',
                        help='interface')
    parser.add_argument('-t', '--timeout', action='store', type=int, default=4,
                        help='Cnx wait delay, 0 for none')
    parser.add_argument('-v','--verbose', action='store_true',
                        help='Increase output verbosity')
    arg = parser.parse_args(sys.argv[1:])

    btle.Debugging = arg.verbose

    central = btle.Central(arg.interface)

    central.gatts = btle_gatts.Gatts()

    central.gatts.addService("Device Information")
    central.gatts.addChar("Model Number String", "My Central")
    central.gatts.addChar("Serial Number String", "3.14159")
    central.gatts.addChar("Firmware Revision String", "2.71828")
    central.gatts.addChar("Hardware Revision String", "1.618")
    central.gatts.addChar("Software Revision String", "1.41421")
    central.gatts.addChar("Manufacturer Name String", "Bluepy")

    central.gatts.addService("Battery Service")
    (bat_chr, bat_val) = central.gatts.addChar("Battery Level", chr(66))
    cccd = central.gatts.addDesc("Client Characteristic Configuration", "\x00\x00")
    cccd.write = MethodType(cccd_write, cccd, btle_gatts.Attribute)
    cccd.read = MethodType(read_not_supported, cccd, btle_gatts.Attribute)
    
    central.start()

    rsp = central._mgmtCmd("info")
    print("Local device bda: %s (%d)"%(rsp["addr"][0], rsp["type"][0]))


    while True:
        central.advertise()
        dev = central.wait_conn(arg.timeout)
        if dev is None:
            break
        central.poll()
        
        # example of dummy notification on battery level
        central._writeCmd("gatts %s\n" % binascii.b2a_hex(bat_val.notification(chr(67))))


    central.stop()
