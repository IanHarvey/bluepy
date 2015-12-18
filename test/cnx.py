#!/usr/bin/env python
import os
import sys
import binascii
import time
# Add btle.py path for import
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'bluepy')))
import btle
import btle_gatts

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


if __name__ == "__main__":
    import sys
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument('host', action='store',
                        help='BD address of BT device')
    parser.add_argument('-p', '--public', action='store_true',
                        help='Indicate BD address is public (default is random)')
    parser.add_argument('-v','--verbose', action='store_true',
                        help='Increase output verbosity')
    arg = parser.parse_args(sys.argv[1:])

    btle.Debugging = arg.verbose


    if arg.public:
        type = btle.ADDR_TYPE_PUBLIC
    else:
        type = btle.ADDR_TYPE_RANDOM

    # connect to device
    device = btle.Peripheral()

    device.gatts = btle_gatts.Gatts()

    device.gatts.addService("Device Information")

    device.gatts.addChar("Model Number String", "Bluepy")
    device.gatts.addChar("Serial Number String", "N/A")
    device.gatts.addChar("Firmware Revision String", "unkown")
    device.gatts.addChar("Hardware Revision String", "unkown")
    device.gatts.addChar("Software Revision String", "0.9.9")
    device.gatts.addChar("Manufacturer Name String", "Github")

    device.gatts.addService("Battery Service")
    device.gatts.addChar("Battery Level", chr(66))

    device.connect(arg.host, type)

    device._mgmtCmd("le on")
    rsp = device._getResp('mgmt')
    if rsp['code'][0] != 'success':
        device._stopHelper()
        raise BTLEException(BTLEException.DISCONNECTED,
                            "Failed to connect to peripheral %s, addr type: %s" % (arg.host, type))

    device._writeCmd("pairable on\n")
    rsp = device._getResp('mgmt')
    if rsp['code'][0] != 'success':
        device._stopHelper()
        raise BTLEException(BTLEException.DISCONNECTED,
                            "Failed to connect to peripheral %s, addr type: %s" % (arg.host, type))

    #device.setSecurityLevel("medium")

    #device.dump_services()

    # discover
    device.discoverServices()
    #device.discover('dis')
    device.dis = device.getServiceByUUID(0x180A)
    device.dismnodel = device.dis.getCharacteristics(0x2A24)[0]
    device.dissn = device.dis.getCharacteristics(0x2A25)[0]
    device.disfwrev = device.dis.getCharacteristics(0x2A26)[0]
    device.dishwrev = device.dis.getCharacteristics(0x2A27)[0]
    device.disswrev = device.dis.getCharacteristics(0x2A28)[0]
    device.dismanuf = device.dis.getCharacteristics(0x2A29)[0]


    while True:
        print "DIS:"
        print "\tMan:", device.dismanuf.read()
        print "\tMod:", device.dismnodel.read()
        print "\tSN: ", device.dissn.read()
        print "\tHwR:", device.dishwrev.read()
        print "\tFwR:", device.disfwrev.read()
        print "\tSwR:", device.disswrev.read()

        device._getResp('', 5)
        #time.sleep(5)


    device.disconnect()
    sys.exit(0)

