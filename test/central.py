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
    parser.add_argument('-c', '--controller', action='store', default='hci0',
                        help='controller')
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
    parser.add_argument('-p', '--public', action='store_true',
                        help='Indicate BD address is public (default is random)')
    parser.add_argument('host', action='store',
                        help='BD address of BT device')
    arg = parser.parse_args(sys.argv[1:])

    btle.Debugging = arg.verbose

    if arg.public:
        addrType = btle.ADDR_TYPE_PUBLIC
    else:
        addrType = btle.ADDR_TYPE_RANDOM

    peripheral = btle.Peripheral(iface = arg.controller)

    peripheral.gatts = btle_gatts.Gatts()

    peripheral.gatts.addService("Device Information")
    peripheral.gatts.addChar("Model Number String", "My Central")
    peripheral.gatts.addChar("Serial Number String", "3.14159")
    peripheral.gatts.addChar("Firmware Revision String", "2.71828")
    peripheral.gatts.addChar("Hardware Revision String", "1.618")
    peripheral.gatts.addChar("Software Revision String", "1.41421")
    peripheral.gatts.addChar("Manufacturer Name String", "Bluepy")

    peripheral.gatts.addService("Battery Service")
    peripheral.gatts.addChar("Battery Level", chr(66))

    peripheral.connect(arg.host, addrType)

    peripheral._getResp("stat", 1000)


