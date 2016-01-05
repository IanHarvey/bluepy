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

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
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
    central.gatts.addChar("Battery Level", chr(66))


    while True:
        central.start()
        central.advertise()
        central.wait_conn()
        central.poll()
