from __future__ import print_function

"""Bluetooth Low Energy Python interface"""
import sys
import os
import time
import subprocess
import binascii
import select

Debugging = False
helperExe = os.path.join(os.path.abspath(os.path.dirname(__file__)), "bluepy-helper")

SEC_LEVEL_LOW = "low"
SEC_LEVEL_MEDIUM = "medium"
SEC_LEVEL_HIGH = "high"

ADDR_TYPE_PUBLIC = "public"
ADDR_TYPE_RANDOM = "random"

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
    # Currently only READ is used in supportsRead function,
    # the rest is included to facilitate supportsXXXX functions if required
    props = {"BROADCAST":    0b00000001,
             "READ":         0b00000010,
             "WRITE_NO_RESP":0b00000100,
             "WRITE":        0b00001000,
             "NOTIFY":       0b00010000,
             "INDICATE":     0b00100000,
             "WRITE_SIGNED": 0b01000000,
             "EXTENDED":     0b10000000,
    }

    propNames = {0b00000001 : "BROADCAST",
                 0b00000010 : "READ",
                 0b00000100 : "WRITE NO RESPONSE",
                 0b00001000 : "WRITE",
                 0b00010000 : "NOTIFY",
                 0b00100000 : "INDICATE",
                 0b01000000 : "WRITE SIGNED",
                 0b10000000 : "EXTENDED PROPERTIES",
    }

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

    def supportsRead(self):
        if (self.properties & Characteristic.props["READ"]):
            return True
        else:
            return False

    def propertiesToString(self):
        propStr = ""
        for p in Characteristic.propNames:
           if (p & self.properties):
               propStr += Characteristic.propNames[p] + " "
        return propStr

    def getHandle(self):
        return self.valHandle

class Descriptor:
    def __init__(self, *args):
        (self.peripheral, uuidVal, self.handle) = args
        self.uuid = UUID(uuidVal)

    def __str__(self):
        return "Descriptor <%s>" % self.uuid.getCommonName()

class DefaultDelegate:
    def __init__(self):
        pass

    def handleNotification(self, cHandle, data):
        DBG("Notification:", cHandle, "sent data", binascii.b2a_hex(data))


class Peripheral:
    def __init__(self, deviceAddr=None, addrType=ADDR_TYPE_PUBLIC):
        self._helper = None
        self._poller = None
        self.services = {} # Indexed by UUID
        self.addrType = addrType
        self.discoveredAllServices = False
        self.delegate = DefaultDelegate()
        if deviceAddr is not None:
            self.connect(deviceAddr, addrType)

    def setDelegate(self, delegate_):
        self.delegate = delegate_

    def _startHelper(self):
        if self._helper is None:
            DBG("Running ", helperExe)
            self._helper = subprocess.Popen([helperExe],
                                            stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE,
                                            universal_newlines=True)
            self._poller = select.poll()
            self._poller.register(self._helper.stdout, select.POLLIN)

    def _stopHelper(self):
        if self._helper is not None:
            DBG("Stopping ", helperExe)
            self._poller.unregister(self._helper.stdout)
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

    def _getResp(self, wantType, timeout=None):
        while True:
            if self._helper.poll() is not None:
                raise BTLEException(BTLEException.INTERNAL_ERROR, "Helper exited")

            if timeout:
                fds = self._poller.poll(timeout*1000)
                if len(fds) == 0:
                    DBG("Select timeout")
                    return None

            rv = self._helper.stdout.readline()
            DBG("Got:", repr(rv))
            if rv.startswith('#'):
                continue

            resp = Peripheral.parseResp(rv)
            if 'rsp' not in resp:
                raise BTLEException(BTLEException.INTERNAL_ERROR,
                                "No response type indicator")
            respType = resp['rsp'][0]
            if respType == 'ntfy':
                hnd = resp['hnd'][0]
                data = resp['d'][0]
                if self.delegate is not None:
                    self.delegate.handleNotification(hnd, data)
                if wantType != respType:
                    continue
                    
            if respType == 'ind':
                hnd = resp['hnd'][0]
                data = resp['d'][0]
                self.delegate.handleNotification(hnd, data)
                if wantType != respType:
                    continue

            if respType == wantType:
                return resp
            elif respType == 'stat' and resp['state'][0] == 'disc':
                self._stopHelper()
                raise BTLEException(BTLEException.DISCONNECTED, "Device disconnected")
            elif respType == 'err':
                errcode=resp['code'][0]
                raise BTLEException(BTLEException.COMM_ERROR, "Error from Bluetooth stack (%s)" % errcode)
            else:
                raise BTLEException(BTLEException.INTERNAL_ERROR, "Unexpected response (%s)" % respType)

    def status(self):
        self._writeCmd("stat\n")
        return self._getResp('stat')

    def connect(self, addr, addrType):
        if len(addr.split(":")) != 6:
            raise ValueError("Expected MAC address, got %s" % repr(addr))
        if addrType not in (ADDR_TYPE_PUBLIC, ADDR_TYPE_RANDOM):
            raise ValueError("Expected address type public or random, got {}".format(addrType))
        self._startHelper()
        self.deviceAddr = addr
        self._writeCmd("conn %s %s\n" % (addr, addrType))
        rsp = self._getResp('stat')
        while rsp['state'][0] == 'tryconn':
            rsp = self._getResp('stat')
        if rsp['state'][0] != 'conn':
            self._stopHelper()
            raise BTLEException(BTLEException.DISCONNECTED,
                                "Failed to connect to peripheral %s, addr type: %s" % (addr, addrType))

    def disconnect(self):
        if self._helper is None:
            return
        # Unregister the delegate first
        self.setDelegate(None)

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
        descriptors = []
        self._writeCmd("desc %X %X\n" % (startHnd, endHnd) )
        # Certain Bluetooth LE devices are not capable of sending back all
        # descriptors in one packet due to the limited size of MTU. So the
        # guest needs to check the response and make retries until all handles
        # are returned.
        # In bluez 5.25, gatt_discover_desc() in attrib/gatt.c does the retry
        # so bluetooth_helper always returns a full list.
        # In bluez 5.4, gatt_find_info() does not make the retry anymore but
        # bluetooth_helper does. However bluetoth_helper returns the handles in
        # multiple response so here we need to wait until all of them are returned
        while len(descriptors) < endHnd - startHnd + 1:
            resp = self._getResp('desc')
            nDesc = len(resp['hnd'])
            descriptors += [Descriptor(self, resp['uuid'][i], resp['hnd'][i]) for i in range(nDesc)]
        return descriptors

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

    def waitForNotifications(self, timeout):
         resp = self._getResp('ntfy', timeout)
         return (resp != None)

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
    UUID(0x181B, "Body Composition"),
    UUID(0x181E, "Bond Management"),
    UUID(0x181F, "Continuous Glucose Monitoring"),
    UUID(0x1805, "Current Time Service"),
    UUID(0x1818, "Cycling Power"),
    UUID(0x1816, "Cycling Speed and Cadence"),
    UUID(0x180A, "Device Information"),
    UUID(0x181A, "Environmental Sensing"),
    UUID(0x1800, "Generic Access"),
    UUID(0x1801, "Generic Attribute"),
    UUID(0x1808, "Glucose"),
    UUID(0x1809, "Health Thermometer"),
    UUID(0x180D, "Heart Rate"),
    UUID(0x1812, "Human Interface Device"),
    UUID(0x1802, "Immediate Alert"),
    UUID(0x1820, "Internet Protocol Support"),
    UUID(0x1803, "Link Loss"),
    UUID(0x1819, "Location and Navigation"),
    UUID(0x1807, "Next DST Change Service"),
    UUID(0x180E, "Phone Alert Status Service"),
    UUID(0x1806, "Reference Time Update Service"),
    UUID(0x1814, "Running Speed and Cadence"),
    UUID(0x1813, "Scan Parameters"),
    UUID(0x1804, "Tx Power"),
    UUID(0x181C, "User Data"),
    UUID(0x181D, "Weight Scale"),

    # Characteristic UUIDs
    UUID(0x2A7E, "Aerobic Heart Rate Lower Limit"),
    UUID(0x2A84, "Aerobic Heart Rate Upper Limit"),
    UUID(0x2A7F, "Aerobic Threshold"),
    UUID(0x2A80, "Age"),
    UUID(0x2A5A, "Aggregate"),
    UUID(0x2A42, "Alert Category ID Bit Mask"),
    UUID(0x2A43, "Alert Category ID"),
    UUID(0x2A06, "Alert Level"),
    UUID(0x2A44, "Alert Notification Control Point"),
    UUID(0x2A3F, "Alert Status"),
    UUID(0x2A81, "Anaerobic Heart Rate Lower Limit"),
    UUID(0x2A82, "Anaerobic Heart Rate Upper Limit"),
    UUID(0x2A83, "Anaerobic Threshold"),
    UUID(0x2A58, "Analog"),
    UUID(0x2A73, "Apparent Wind Direction"),
    UUID(0x2A72, "Apparent Wind Speed"),
    UUID(0x2A01, "Appearance"),
    UUID(0x2AA3, "Barometric Pressure Trend"),
    UUID(0x2A19, "Battery Level"),
    UUID(0x2A49, "Blood Pressure Feature"),
    UUID(0x2A35, "Blood Pressure Measurement"),
    UUID(0x2A9B, "Body Composition Feature"),
    UUID(0x2A9C, "Body Composition Measurement"),
    UUID(0x2A38, "Body Sensor Location"),
    UUID(0x2AA4, "Bond Management Control Point"),
    UUID(0x2AA5, "Bond Management Feature"),
    UUID(0x2A22, "Boot Keyboard Input Report"),
    UUID(0x2A32, "Boot Keyboard Output Report"),
    UUID(0x2A33, "Boot Mouse Input Report"),
    UUID(0x2AA8, "CGM Feature"),
    UUID(0x2AA7, "CGM Measurement"),
    UUID(0x2AAB, "CGM Session Run Time"),
    UUID(0x2AAA, "CGM Session Start Time"),
    UUID(0x2AAC, "CGM Specific Ops Control Point"),
    UUID(0x2AA9, "CGM Status"),
    UUID(0x2A5C, "CSC Feature"),
    UUID(0x2A5B, "CSC Measurement"),
    UUID(0x2AA6, "Central Address Resolution"),
    UUID(0x2905, "Characteristic Aggregate Formate"),
    UUID(0x2900, "Characteristic Extended Properties"),
    UUID(0x2904, "Characteristic Format"),
    UUID(0x2901, "Characteristic User Description"),
    UUID(0x2803, "Characteristic"),
    UUID(0x2902, "Client Characteristic Configuration"),
    UUID(0x2A2B, "Current Time"),
    UUID(0x2A66, "Cycling Power Control Point"),
    UUID(0x2A65, "Cycling Power Feature"),
    UUID(0x2A63, "Cycling Power Measurement"),
    UUID(0x2A64, "Cycling Power Vector"),
    UUID(0x2A0D, "DST Offset"),
    UUID(0x2A99, "Database Change Increment"),
    UUID(0x2A08, "Date Time"),
    UUID(0x2A85, "Date of Birth"),
    UUID(0x2A86, "Date of Threshold Assessment"),
    UUID(0x2A0A, "Day Date Time"),
    UUID(0x2A09, "Day of Week"),
    UUID(0x2A7D, "Descriptor Value Changed"),
    UUID(0x2A00, "Device Name"),
    UUID(0x2A7B, "Dew Point"),
    UUID(0x2A56, "Digital"),
    UUID(0x2A6C, "Elevation"),
    UUID(0x2A87, "Email Address"),
    UUID(0x290B, "Environmental Sensing Configuration"),
    UUID(0x290C, "Environmental Sensing Measurement"),
    UUID(0x290D, "Environmental Sensing Trigger Setting"),
    UUID(0x2A0C, "Exact Time 256"),
    UUID(0x2907, "External Report Reference"),
    UUID(0x2A88, "Fat Burn Heart Rate Lower Limit"),
    UUID(0x2A89, "Fat Burn Heart Rate Upper Limit"),
    UUID(0x2A26, "Firmware Revision String"),
    UUID(0x2A8A, "First Name"),
    UUID(0x2A8B, "Five Zone Heart Rate Limits"),
    UUID(0x2A8C, "Gender"),
    UUID(0x2A51, "Glucose Feature"),
    UUID(0x2A34, "Glucose Measurement Context"),
    UUID(0x2A18, "Glucose Measurement"),
    UUID(0x2A74, "Gust Factor"),
    UUID(0x2A4C, "HID Control Point"),
    UUID(0x2A4A, "HID Information"),
    UUID(0x2A27, "Hardware Revision String"),
    UUID(0x2A39, "Heart Rate Control Point"),
    UUID(0x2A8D, "Heart Rate Max"),
    UUID(0x2A37, "Heart Rate Measurement"),
    UUID(0x2A7A, "Heat Index"),
    UUID(0x2A8E, "Height"),
    UUID(0x2A8F, "Hip Circumference"),
    UUID(0x2A6F, "Humidity"),
    UUID(0x2A2A, "IEEE 11073-20601 Regulatory Certification Data List"),
    UUID(0x2A2A, "IEEE 11073-20601 Regulatory Cert. Data List"),
    UUID(0x2802, "Include"),
    UUID(0x2A36, "Intermediate Cuff Pressure"),
    UUID(0x2A1E, "Intermediate Temperature"),
    UUID(0x2A77, "Irradiance"),
    UUID(0x2A6B, "LN Control Point"),
    UUID(0x2A6A, "LN Feature"),
    UUID(0x2AA2, "Language"),
    UUID(0x2A90, "Last Name"),
    UUID(0x2A0F, "Local Time Information"),
    UUID(0x2A67, "Location and Speed"),
    UUID(0x2A2C, "Magnetic Declination"),
    UUID(0x2AA0, "Magnetic Flux Density - 2D"),
    UUID(0x2AA1, "Magnetic Flux Density - 3D"),
    UUID(0x2A29, "Manufacturer Name String"),
    UUID(0x2A91, "Maximum Recommended Heart Rate"),
    UUID(0x2A21, "Measurement Interval"),
    UUID(0x2A24, "Model Number String"),
    UUID(0x2A68, "Navigation"),
    UUID(0x2A46, "New Alert"),
    UUID(0x2909, "Number of Digitals"),
    UUID(0x2A04, "Peripheral Preferred Connection Parameters"),
    UUID(0x2A02, "Peripheral Privacy Flag"),
    UUID(0x2A50, "PnP ID"),
    UUID(0x2A75, "Pollen Concentration"),
    UUID(0x2A69, "Position Quality"),
    UUID(0x2A6D, "Pressure"),
    UUID(0x2800, "Primary Service"),
    UUID(0x2A4E, "Protocol Mode"),
    UUID(0x2A54, "RSC Feature"),
    UUID(0x2A53, "RSC Measurement"),
    UUID(0x2A78, "Rainfall"),
    UUID(0x2A03, "Reconnection Address"),
    UUID(0x2A52, "Record Access Control Point"),
    UUID(0x2A14, "Reference Time Information"),
    UUID(0x2A4B, "Report Map"),
    UUID(0x2908, "Report Reference"),
    UUID(0x2A4D, "Report"),
    UUID(0x2A92, "Resting Heart Rate"),
    UUID(0x2A40, "Ringer Control Point"),
    UUID(0x2A41, "Ringer Setting"),
    UUID(0x2A55, "SC Control Point"),
    UUID(0x2A4F, "Scan Interval Window"),
    UUID(0x2A31, "Scan Refresh"),
    UUID(0x2801, "Secondary Service"),
    UUID(0x2A5D, "Sensor Location"),
    UUID(0x2A25, "Serial Number String"),
    UUID(0x2903, "Server Characteristic Configuration"),
    UUID(0x2A05, "Service Changed"),
    UUID(0x2A28, "Software Revision String"),
    UUID(0x2A93, "Sport Type for Aerobic/Anaerobic Thresholds"),
    UUID(0x2A47, "Supported New Alert Category"),
    UUID(0x2A48, "Supported Unread Alert Category"),
    UUID(0x2A23, "System ID"),
    UUID(0x2A1C, "Temperature Measurement"),
    UUID(0x2A1D, "Temperature Type"),
    UUID(0x2A6E, "Temperature"),
    UUID(0x2A94, "Three Zone Heart Rate Limits"),
    UUID(0x2A12, "Time Accuracy"),
    UUID(0x2A13, "Time Source"),
    UUID(0x290E, "Time Trigger Setting"),
    UUID(0x2A16, "Time Update Control Point"),
    UUID(0x2A17, "Time Update State"),
    UUID(0x2A0E, "Time Zone"),
    UUID(0x2A11, "Time with DST"),
    UUID(0x2A7C, "Trend"),
    UUID(0x2A71, "True Wind Direction"),
    UUID(0x2A70, "True Wind Speed"),
    UUID(0x2A95, "Two Zone Heart Rate Limit"),
    UUID(0x2A07, "Tx Power Level"),
    UUID(0x2A76, "UV Index"),
    UUID(0x2A45, "Unread Alert Status"),
    UUID(0x2A9F, "User Control Point"),
    UUID(0x2A9A, "User Index"),
    UUID(0x2A96, "VO2 Max"),
    UUID(0x2906, "Valid Range"),
    UUID(0x290A, "Value Trigger Setting"),
    UUID(0x2A97, "Waist Circumference"),
    UUID(0x2A9D, "Weight Measurement"),
    UUID(0x2A9E, "Weight Scale Feature"),
    UUID(0x2A98, "Weight"),
    UUID(0x2A79, "Wind Chill"),
    ])


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit("Usage:\n  %s <mac-address> [random]" % sys.argv[0])

    if not os.path.isfile(helperExe):
        raise ImportError("Cannot find required executable '%s'" % helperExe)

    devAddr = sys.argv[1]
    if len(sys.argv) == 3:
        addrType = sys.argv[2]
    else:
        addrType = ADDR_TYPE_PUBLIC
    print("Connecting to: {}, address type: {}".format(devAddr, addrType))
    conn = Peripheral(devAddr, addrType)
    try:
        for svc in conn.getServices():
            print(str(svc), ":")
            for ch in svc.getCharacteristics():
                print("    {}, supports {}".format(ch, ch.propertiesToString()))
                chName = AssignedNumbers.getCommonName(ch.uuid)
                if (ch.supportsRead()):
                    try:
                        print("    ->", repr(ch.read()))
                    except BTLEException as e:
                        print("    ->", e)

    finally:
        conn.disconnect()
