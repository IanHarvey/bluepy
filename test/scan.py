#!/usr/bin/env python
import argparse
import binascii
import time
import os
import sys
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


DATA_TYPES = {
    1 : 'Flags',
    2 : 'Incomplete 16b Services',
    3 : 'Complete 16b Services',
    4 : 'Incomplete 32b Services',
    5 : 'Complete 32b Services',
    6 : 'Incomplete 128b Services',
    7 : 'Complete 128b Services',
    8 : 'Short Local Name',
    9 : 'Complete Local Name',
    0xA : 'Tx Power',
    0x14 : '16b Service Solicitation',
    0x1F : '32b Service Solicitation',
    0x15 : '128b Service Solicitation',
    0x16 : '16b Service Data',
    0x20 : '32b Service Data',
    0x21 : '128b Service Data',
    0x17 : 'Public Target Address',
    0x18 : 'Random Target Address',
    0x19 : 'Appearance',
    0x1A : 'Advertising Interval',
    0xFF : 'Manufacturer',
}


def mac(addr):
    addr = binascii.b2a_hex(addr)
    return ':'.join([ addr[2*i:2*i+2] for i in xrange(6)])

def dump_services(dev):
    services = sorted(dev.getServices(), key=lambda s: s.hndStart)
    for s in services:
        print "\t%04x: %s" % (s.hndStart, s)
        if s.hndStart == s.hndEnd:
            continue
        chars = s.getCharacteristics()
        for i, c in enumerate(chars):
            props = c.propertiesToString()
            h = c.getHandle()
            print "\t%04x:    %-59s %-12s" % (h, c, props),
            if 'READ' in props:
                val = c.read()
                if c.uuid == btle.AssignedNumbers.device_name:
                    string = ANSI_CYAN + '\'' + val + '\'' + ANSI_OFF
                elif s.uuid == btle.AssignedNumbers.device_information:
                    string = '\'' + val + '\''
                else:
                    string = '<' + binascii.hexlify(val) + '>'
            else:
                string=''
            print string

            while True:
                h += 1
                if h > s.hndEnd or (i < len(chars) -1 and h >= chars[i+1].getHandle() - 1):
                    break
                try:
                    val = dev.readCharacteristic(h)
                    print "\t%04x:     <%s>" % (h, binascii.hexlify(val))
                except btle.BTLEException:
                    break

def scan_cb(entry, addr, type, rssi, connectable, data, data_raw):
    rssi_val = sum(rssi) / len(rssi)
    if (entry == 'old' and not arg.all) or (entry == 'update' and arg.new) or (rssi_val < arg.sensitivity):
        return
    print '    Device (%s):' % entry, ANSI_WHITE + mac(addr) + ANSI_OFF, '(' + type + ')  ', \
        rssi_val, 'dBm', \
        ('' if connectable else '(not connectable)')
    for id,v in data.iteritems():
        if id in [8,9]:
            print '\t' + DATA_TYPES[id] + ': \'' + ANSI_CYAN + v + ANSI_OFF + '\''
        elif id in DATA_TYPES:
            print '\t' + DATA_TYPES[id] + ': <' + binascii.b2a_hex(v) + '>'
        else:
            print '\tid 0x%x: <' + binascii.b2a_hex(v) + '>'
    if not data:
        print '\t(no data)'
    print

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    #parser.add_argument('host', action='store',
    #                    help='BD address of BT device')
    parser.add_argument('-t', '--timeout', action='store', type=int, default=4,
                        help='Scan delay, 0 for continuous')
    parser.add_argument('-s', '--sensitivity', action='store', type=int, default=-128,
                        help='dBm value for filtering far devices')
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

    scanner = btle.Scanner()

    print ANSI_RED + "Scanning for devices..." + ANSI_OFF
    devices = scanner.scan(arg.timeout, scan_cb)

    if arg.discover:
        print ANSI_RED + "Discovering services..." + ANSI_OFF

        for addr,d in devices.iteritems():
            if not d['connectable']:
                continue

            print "    Connecting to", ANSI_WHITE + mac(addr) + ANSI_OFF + ":"

            dev = btle.Peripheral(mac(addr), d['type'])
            dump_services(dev)
            dev.disconnect()
            print





