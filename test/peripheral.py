#!/usr/bin/env python
import argparse
import binascii
import time
import os
import sys
# Add btle.py path for import
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'bluepy')))
import btle

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
    arg = parser.parse_args(sys.argv[1:])

    btle.Debugging = arg.verbose

    central = btle.Central(arg.controller)
    central.start()
    central.advertise()
    central.wait_conn()
    
