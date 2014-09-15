from __future__ import print_function

"""Bluetooth Low Energy Python interface"""
import sys
import os
import time
import subprocess
import binascii

Debugging = False
helperExe = os.path.join(os.path.abspath(os.path.dirname(__file__)), "bluepy-helper")

SEC_LEVEL_LOW = "low"
SEC_LEVEL_MEDIUM = "medium"
SEC_LEVEL_HIGH = "high"


def DBG(*args):
    if Debugging:
        msg = " ".join([str(a) for a in args])
        print(msg)


class BTLEException(Exception):

    """BTLE Exception."""

    DISCONNECTED = 1
    COMM_ERROR = 2
    INTERNAL_ERROR = 3

    def __init__(self, code, message):
        self.code = code
        self.message = message

    def __str__(self):
        return self.message


class UUID:
    def __init__(self, val, commonName=None):
        '''We accept: 32-digit hex strings, with and without '-' characters,
           4 to 8 digit hex strings, and integers'''
        if isinstance(val, int):
            if (val < 0) or (val > 0xFFFFFFFF):
                raise ValueError(
                    "Short form UUIDs must be in range 0..0xFFFFFFFF")
            val = "%04X" % val
        elif isinstance(val, self.__class__):
            val = str(val)
        else:
            val = str(val)  # Do our best

        val = val.replace("-", "")
        if len(val) <= 8:  # Short form
            val = ("0" * (8 - len(val))) + val + "00001000800000805F9B34FB"

        self.binVal = binascii.a2b_hex(val)
        if len(self.binVal) != 16:
            raise ValueError(
                "UUID must be 16 bytes, got '%s' (len=%d)" % (val,
                                                              len(self.binVal)))
        self.commonName = commonName

    def __str__(self):
        s = binascii.b2a_hex(self.binVal).decode('utf-8')
        return "-".join([s[0:8], s[8:12], s[12:16], s[16:20], s[20:32]])

    def __eq__(self, other):
        return self.binVal == UUID(other).binVal

    def __cmp__(self, other):
        return cmp(self.binVal, UUID(other).binVal)

    def __hash__(self):
        return hash(self.binVal)

    def getCommonName(self):
        s = AssignedNumbers.getCommonName(self)
        if s:
            return s
        s = str(self)
        if s.endswith("-0000-1000-8000-00805f9b34fb"):
            s = s[0:8]
            if s.startswith("0000"):
                s = s[4:]    
        return s

class Service:
    def __init__(self, *args):
        (self.peripheral, uuidVal, self.hndStart, self.hndEnd) = args
        self.uuid = UUID(uuidVal)
        self.chars = None

    def getCharacteristics(self, forUUID=None):
        if not self.chars: # Unset, or empty
            self.chars = self.peripheral.getCharacteristics(self.hndStart, self.hndEnd)
        if forUUID is not None:
            u = UUID(forUUID)
            return [ch for ch in self.chars if ch.uuid==u]
        return self.chars

    def __str__(self):
        return "Service <uuid=%s handleStart=%s handleEnd=%s>" % (self.uuid.getCommonName(),
                                                                 self.hndStart,
                                                                 self.hndEnd)

class Characteristic:
    def __init__(self, *args):
        (self.peripheral, uuidVal, self.handle, self.properties, self.valHandle) = args
        self.uuid = UUID(uuidVal)

    def read(self):
        return self.peripheral.readCharacteristic(self.valHandle)

    def write(self, val, withResponse=False):
        self.peripheral.writeCharacteristic(self.valHandle, val, withResponse)

    # TODO: descriptors

    def __str__(self):
        return "Characteristic <%s>" % self.uuid.getCommonName()

class Descriptor:
    def __init__(self, *args):
        (self.peripheral, uuidVal, self.handle) = args
        self.uuid = UUID(uuidVal)

    def __str__(self):
        return "Descriptor <%s>" % self.uuid.getCommonName()

class Peripheral:
    def __init__(self, deviceAddr=None):
        self._helper = None
        self.services = {} # Indexed by UUID
        self.discoveredAllServices = False
        if deviceAddr is not None:
            self.connect(deviceAddr)

    def _startHelper(self):
        if self._helper is None:
            DBG("Running ", helperExe)
            self._helper = subprocess.Popen([helperExe],
                                            stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE,
                                            universal_newlines=True)

    def _stopHelper(self):
        if self._helper is not None:
            DBG("Stopping ", helperExe)
            self._helper.stdin.write("quit\n")
            self._helper.stdin.flush()
            self._helper.wait()
            self._helper = None

    def _writeCmd(self, cmd):
        if self._helper is None:
            raise BTLEException(BTLEException.INTERNAL_ERROR,
                                "Helper not started (did you call connect()?)")
        DBG("Sent: ", cmd)
        self._helper.stdin.write(cmd)
        self._helper.stdin.flush()


    @staticmethod
    def parseResp(line):
        resp = {}
        for item in line.rstrip().split(' '):
            (tag, tval) = item.split('=')
            if len(tval)==0:
                val = None
            elif tval[0]=="$" or tval[0]=="'":
                # Both symbols and strings as Python strings
                val = tval[1:]
            elif tval[0]=="h":
                val = int(tval[1:], 16)
            elif tval[0]=='b':
                val = binascii.a2b_hex(tval[1:])
            else:
                raise BTLEException(BTLEException.INTERNAL_ERROR,
                             "Cannot understand response value %s" % repr(tval))
            if tag not in resp:
                resp[tag] = [val]
            else:
                resp[tag].append(val)
        return resp

    def _getResp(self, wantType):
        while True:
            if self._helper.poll() is not None:
                raise BTLEException(BTLEException.INTERNAL_ERROR, "Helper exited")

            rv = self._helper.stdout.readline()
            DBG("Got:", repr(rv))
            if rv.startswith('#'):
                continue

            resp = Peripheral.parseResp(rv)
            if 'rsp' not in resp:
                raise BTLEException(BTLEException.INTERNAL_ERROR,
                                "No response type indicator")
            respType = resp['rsp'][0]
            if respType == wantType:
                return resp
            elif respType == 'stat' and resp['state'][0] == 'disc':
                self._stopHelper()
                raise BTLEException(BTLEException.DISCONNECTED, "Device disconnected")
            elif respType == 'err':
                errcode=resp['code'][0]
                raise BTLEException(BTLEException.COMM_ERROR, "Error from Bluetooth stack (%s)" % errcode)
            elif respType == 'ntfy':
                DBG("Ignoring notification")
                continue
            else:
                raise BTLEException(BTLEException.INTERNAL_ERROR, "Unexpected response (%s)" % respType)

    def status(self):
        self._writeCmd("stat\n")
        return self._getResp('stat')

    def connect(self, addr):
        if len(addr.split(":")) != 6:
            raise ValueError("Expected MAC address, got %s", repr(addr))
        self._startHelper()
        self.deviceAddr = addr
        self._writeCmd("conn %s\n" % addr)
        rsp = self._getResp('stat')
        while rsp['state'][0] == 'tryconn':
            rsp = self._getResp('stat')
        if rsp['state'][0] != 'conn':
            self._stopHelper()
            raise BTLEException(BTLEException.DISCONNECTED,
                                "Failed to connect to peripheral %s" % addr)

    def disconnect(self):
        if self._helper is None:
            return
        self._writeCmd("disc\n")
        self._getResp('stat')
        self._stopHelper()

    def discoverServices(self):
        self._writeCmd("svcs\n")
        rsp = self._getResp('find')
        starts = rsp['hstart']
        ends   = rsp['hend']
        uuids  = rsp['uuid']
        nSvcs = len(uuids)
        assert(len(starts)==nSvcs and len(ends)==nSvcs)
        self.services = {}
        for i in range(nSvcs):
            self.services[UUID(uuids[i])] = Service(self, uuids[i], starts[i], ends[i])
        self.discoveredAllServices = True
        return self.services

    def getServices(self):
        if not self.discoveredAllServices:
            self.discoverServices()
        return self.services.values()

    def getServiceByUUID(self, uuidVal):
        uuid = UUID(uuidVal)
        if uuid in self.services:
            return self.services[uuid]
        self._writeCmd("svcs %s\n" % uuid)
        rsp = self._getResp('find')
        svc = Service(self, uuid, rsp['hstart'][0], rsp['hend'][0])
        self.services[uuid] = svc
        return svc

    def _getIncludedServices(self, startHnd=1, endHnd=0xFFFF):
        # TODO: No working example of this yet
        self._writeCmd("incl %X %X\n" % (startHnd, endHnd))
        return self._getResp('find')

    def getCharacteristics(self, startHnd=1, endHnd=0xFFFF, uuid=None):
        cmd = 'char %X %X' % (startHnd, endHnd)
        if uuid:
            cmd += ' %s' % UUID(uuid)
        self._writeCmd(cmd + "\n")
        rsp = self._getResp('find')
        nChars = len(rsp['hnd'])
        return [Characteristic(self, rsp['uuid'][i], rsp['hnd'][i],
                               rsp['props'][i], rsp['vhnd'][i])
                for i in range(nChars)]

    def getDescriptors(self, startHnd=1, endHnd=0xFFFF):
        self._writeCmd("desc %X %X\n" % (startHnd, endHnd) )
        resp = self._getResp('desc')
        nDesc = len(resp['hnd'])
        return [Descriptor(self, resp['uuid'][i], resp['hnd'][i]) for i in
                range(nDesc)]

    def readCharacteristic(self, handle):
        self._writeCmd("rd %X\n" % handle)
        resp = self._getResp('rd')
        return resp['d'][0]

    def _readCharacteristicByUUID(self, uuid, startHnd, endHnd):
        # Not used at present
        self._writeCmd("rdu %s %X %X\n" % (UUID(uuid), startHnd, endHnd))
        return self._getResp('rd')

    def writeCharacteristic(self, handle, val, withResponse=False):
        cmd = "wrr" if withResponse else "wr"
        self._writeCmd("%s %X %s\n" % (cmd, handle, binascii.b2a_hex(val).decode('utf-8')))
        return self._getResp('wr')

    def setSecurityLevel(self, level):
        self._writeCmd("secu %s\n" % level)
        return self._getResp('stat')

    def setMTU(self, mtu):
        self._writeCmd("mtu %x\n" % mtu)
        return self._getResp('stat')

    def __del__(self):
        self.disconnect()

def capitaliseName(descr):
    words = descr.split(" ")
    capWords =  [ words[0].lower() ]
    capWords += [ w[0:1].upper() + w[1:].lower() for w in words[1:] ]
    return "".join(capWords)

class _UUIDNameMap:
    # Constructor sets self.currentTimeService, self.txPower, and so on
    # from names.
    def __init__(self, idList):
        self.idMap = {}
        
        for uuid in idList:
            attrName = capitaliseName(uuid.commonName)
            vars(self) [attrName] = uuid
            self.idMap[uuid] = uuid

    def getCommonName(self, uuid):
        if uuid in self.idMap:
            return self.idMap[uuid].commonName
        return None
        
AssignedNumbers = _UUIDNameMap( [    
    # Service UUIDs
    UUID(0x1811, "Alert Notification Service"),
    UUID(0x180F, "Battery Service"),
    UUID(0x1810, "Blood Pressure"),
    UUID(0x1805, "Current Time Service"),
    UUID(0x1818, "Cycling Power"),
    UUID(0x1816, "Cycling Speed and Cadence"),
    UUID(0x180A, "Device Information"),
    UUID(0x1800, "Generic Access"),
    UUID(0x1801, "Generic Attribute"),
    UUID(0x1808, "Glucose"),
    UUID(0x1809, "Health Thermometer"),
    UUID(0x180D, "Heart Rate"),
    UUID(0x1812, "Human Interface Device"),
    UUID(0x1802, "Immediate Alert"),
    UUID(0x1803, "Link Loss"),
    UUID(0x1819, "Location and Navigation"),
    UUID(0x1807, "Next DST Change Service"),
    UUID(0x180E, "Phone Alert Status Service"),
    UUID(0x1806, "Reference Time Update Service"),
    UUID(0x1814, "Running Speed and Cadence"),
    UUID(0x1813, "Scan Parameters"),
    UUID(0x1804, "Tx Power"),
    UUID(0x181C, "User Data"),

    # Characteristic UUIDs
    UUID(0x2A00, "Device Name"),
    UUID(0x2A07, "Tx Power Level"),
    UUID(0x2A19, "Battery Level"),
    UUID(0x2A24, "Model Number String"),
    UUID(0x2A25, "Serial Number String"),
    UUID(0x2A26, "Firmware Revision String"),
    UUID(0x2A27, "Hardware Revision String"),
    UUID(0x2A28, "Software Revision String"),
    UUID(0x2A29, "Manufacturer Name String"),

    ])


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit("Usage:\n  %s <mac-address>" % sys.argv[0])

    if not os.path.isfile(helperExe):
        raise ImportError("Cannot find required executable '%s'" % helperExe)

    Debugging = False
    devaddr = sys.argv[1]
    print("Connecting to:", devaddr)
    conn = Peripheral(devaddr)
    try:
        for svc in conn.getServices():
            print(str(svc), ":")
            for ch in svc.getCharacteristics():
                print("    " + str(ch))
                chName = AssignedNumbers.getCommonName(ch.uuid)
                try:
                    print("    ->", repr(ch.read()))
                except BTLEException as e:
                    print("    ->", e)

    finally:
        conn.disconnect()
