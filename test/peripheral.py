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

    central.gatts = btle_gatts.Gatts(central)

    central.gatts.addService("Device Information")
    central.gatts.addChar("Model Number String", "My Central")
    central.gatts.addChar("Serial Number String", "3.14159")
    central.gatts.addChar("Firmware Revision String", "2.71828")
    central.gatts.addChar("Hardware Revision String", "1.618")
    central.gatts.addChar("Software Revision String", "1.41421")
    central.gatts.addChar("Manufacturer Name String", "Bluepy")

    hr_svc = central.gatts.addService("Heart Rate")
    hr_start = hr_svc.handle

    central.gatts.addChar("Heart Rate Measurement", chr(5))
    (hr_first_char, hr_last_char) = central.gatts.addChar("Heart Rate Control Point", "60")
    hr_end = hr_last_char.handle

    bat_svc = central.gatts.addService("Battery Service")
    hr_incl_svc = central.gatts.addInclService(hr_start, hr_end, "Heart Rate")

    (bat_chr, bat_val) = central.gatts.addChar("Battery Level", chr(66))
    cccd = central.gatts.addDesc("Client Characteristic Configuration", "\x00\x00")
    cccd.write = MethodType(cccd_write, cccd, btle_gatts.Attribute)
    cccd.read = MethodType(read_not_supported, cccd, btle_gatts.Attribute)

    # example of vendor specific UUID
    central.gatts.addService("de305d54-75b4-431b-adb2-eb6b9e546015")
    central.gatts.addChar("de305d54-75b4-431b-adb2-eb6b9e546016", "My private", prop = ["READ", "WRITE", "NOTIFY", "INDICATE"])
    cccd = central.gatts.addDesc("Client Characteristic Configuration", "\x00\x00")

    central.start()

    print("Local device bda: %s (%s)"%(central.addr, central.addr_type))

    while True:
        central.advertise()
        dev = central.wait_conn(arg.timeout)
        if dev is None:
            break

        # uncomment if bonding support required
        # central.setSecurityLevel("medium")

        while True:
            central.poll(3)

            # example of dummy notification on battery level
            bat_val.notify(chr(67))

            central.poll(3)

            # example of dummy indication on battery level
            bat_val.indicate(chr(66))

    central.stop()
