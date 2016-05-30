#!/usr/bin/env python

from __future__ import print_function

"""Bluetooth Low Energy Python interface"""
import sys
import os
import time
import subprocess
import binascii
import select
import struct

Debugging = False
script_path = os.path.join(os.path.abspath(os.path.dirname(__file__)))
helperExe = os.path.join(script_path, "bluepy-helper")

SEC_LEVEL_LOW = "low"
SEC_LEVEL_MEDIUM = "medium"
SEC_LEVEL_HIGH = "high"

ADDR_TYPE_PUBLIC = "public"
ADDR_TYPE_RANDOM = "random"

MGMT_SETTING_POWERED            = 0x00000001
MGMT_SETTING_CONNECTABLE        = 0x00000002
MGMT_SETTING_FAST_CONNECTABLE   = 0x00000004
MGMT_SETTING_DISCOVERABLE       = 0x00000008
MGMT_SETTING_BONDABLE           = 0x00000010
MGMT_SETTING_LINK_SECURITY      = 0x00000020
MGMT_SETTING_SSP                = 0x00000040
MGMT_SETTING_BREDR              = 0x00000080
MGMT_SETTING_HS                 = 0x00000100
MGMT_SETTING_LE                 = 0x00000200
MGMT_SETTING_ADVERTISING        = 0x00000400
MGMT_SETTING_SECURE_CONN        = 0x00000800
MGMT_SETTING_DEBUG_KEYS         = 0x00001000
MGMT_SETTING_PRIVACY            = 0x00002000
MGMT_SETTING_CONFIGURATION      = 0x00004000
MGMT_SETTING_STATIC_ADDRESS     = 0x00008000

def DBG(*args):
    if Debugging:
        msg = " ".join([str(a) for a in args])
        print(msg)


class BTLEException(Exception):

    """BTLE Exception."""

    DISCONNECTED = 1
    COMM_ERROR = 2
    INTERNAL_ERROR = 3
    GATT_ERROR = 4
    MGMT_ERROR = 5

    def __init__(self, code, message, bt_err = 0):
        self.code = code
        self.message = message
        self.bt_err = bt_err

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
            if len(val) <= 4:
                self.binValShort = struct.pack('<H', int(val, 16))
            else:
                self.binValShort = struct.pack('<L', int(val, 16))
            val = ("0" * (8 - len(val))) + val + "00001000800000805F9B34FB"
        else:
            self.binValShort = None

        self.binVal = binascii.a2b_hex(val.encode('utf-8'))[::-1]
        if len(self.binVal) != 16:
            raise ValueError(
                "UUID must be 16 bytes, got '%s' (len=%d)" % (val,
                                                              len(self.binVal)))
        self.commonName = commonName

    def __str__(self):
        s = binascii.b2a_hex(self.binVal[::-1]).decode('utf-8')
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

    def bin(self):
        if self.binValShort is None:
            return self.binVal
        else:
            return self.binValShort

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

    def getIncludedServices(self, startHnd=1, endHnd=0xFFFF):
        return self.peripheral._getIncludedServices(self.hndStart, self.hndEnd)

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

    def read(self, offset=None):
        return self.peripheral.readCharacteristic(self.valHandle, offset)

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

    def handleScan(self, scanEntry, isNewDev, isNewData):
        DBG("Discovered device", scanEntry.addr)

class BluepyHelper:
    def __init__(self, iface='hci0'):
        self._helper = None
        self._poller = None
        self._stderr = None
        self.delegate = DefaultDelegate()
        self._iface = iface

    def withDelegate(self, delegate_):
        self.delegate = delegate_
        return self

    def _startHelper(self):
        if self._helper is None:
            DBG("Running ", helperExe)
            self._stderr = open(os.devnull, "w")
            self._helper = subprocess.Popen([helperExe, self._iface],
                                            stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE,
                                            stderr=self._stderr,
                                            universal_newlines=True)
            self._poller = select.poll()
            self._poller.register(self._helper.stdout, select.POLLIN)
            # retrieve the controller info
            rv = self._mgmtCmd("info")
            if rv == None or rv["code"][0] != "success":
                raise BTLEException(BTLEException.INTERNAL_ERROR,
                                    "Helper not started (did you call connect()?)")
            # Not possible to retrieve current random address (Except via debugfs)
            self.local_public = rv["addr"][0]

    def _stopHelper(self):
        if self._helper is not None:
            DBG("Stopping ", helperExe)
            self._poller.unregister(self._helper.stdout)
            self._helper.stdin.write("quit\n")
            self._helper.stdin.flush()
            self._helper.wait()
            self._helper = None
        if self._stderr is not None:
            self._stderr.close()
            self._stderr = None

    def _writeCmd(self, cmd):
        if self._helper is None:
            raise BTLEException(BTLEException.INTERNAL_ERROR,
                                "Helper not started (did you call connect()?)")
        DBG("Sent: ", cmd)
        self._helper.stdin.write(cmd)
        self._helper.stdin.flush()

    def _mgmtCmd(self, cmd):
        self._writeCmd(cmd + '\n')
        rsp = self._waitResp('mgmt')
        if rsp['code'][0] != 'success':
            self._stopHelper()
            raise BTLEException(BTLEException.DISCONNECTED,
                            "Failed to execute mgmt cmd '%s' : %s" % (cmd, rsp['code'][0]))
        return rsp

    # Reset the interface device. Can be optional as super user permissions are needed
    def reset_controller(self, required = True):
        try:
            rsp = self._mgmtCmd("settings")
            settings = rsp['d'][0]
        except BTLEException as e:
            if e.code == BTLEException.MGMT_ERROR and not required:
                return
            raise e

        if settings & MGMT_SETTING_BREDR:
            # powered off (mandatory to set bredr off)
            self._mgmtCmd("powered off")
            settings &= ~MGMT_SETTING_POWERED
            # first enable LE is not yet enabled
            if (settings & MGMT_SETTING_LE) == 0:
                self._mgmtCmd("le on")
            # set bredr off
            self._mgmtCmd("bredr off")

        # always end advertising if it is active
        if settings & MGMT_SETTING_ADVERTISING:
            self._mgmtCmd("advertising off")
            rsp = self._waitResp('stat', 0.5)
            if rsp['state'][0] != 'disc':
                raise BTLEException(BTLEException.INTERNAL_ERROR,
                                    "Failed to stop advertising")

        # power on the HCI
        if settings & MGMT_SETTING_POWERED == 0:
            self._mgmtCmd("powered on")

        rsp = self._mgmtCmd("settings")
        settings = rsp['d'][0]
        # make sure to launch 'le on' cmd
        if (settings & MGMT_SETTING_LE) == 0:
            self._mgmtCmd("le on")

        # Only public address for local device is supported.
        # Bluez/Linux 3.19 use random addresses in the following cases:
        # - scan request (we do not care)
        # - FORCE STATIC debug setting (only API is debugfs)
        # - PRIVACY setting (not supported)
        # - CONNECTABLE setting when peripheral (not supported)
        assert (settings & MGMT_SETTING_PRIVACY) == 0

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
                val = binascii.a2b_hex(tval[1:].encode('utf-8'))
            else:
                raise BTLEException(BTLEException.INTERNAL_ERROR,
                             "Cannot understand response value %s" % repr(tval))
            if tag not in resp:
                resp[tag] = [val]
            else:
                resp[tag].append(val)
        return resp

    def _waitResp(self, wantType, timeout=None):
        # make sure wantType is a list
        if isinstance(wantType, list) is not True:
            wantType = [wantType]

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
            if rv.startswith('#') or rv == '\n':
                continue

            resp = BluepyHelper.parseResp(rv)
            if 'rsp' not in resp:
                raise BTLEException(BTLEException.INTERNAL_ERROR, "No response type indicator")

            respType = resp['rsp'][0]
            if respType in wantType:
                return resp
            elif respType == 'stat' and resp['state'][0] == 'disc':
                self._stopHelper()
                raise BTLEException(BTLEException.DISCONNECTED, "Device disconnected")
            elif respType == 'err':
                errcode=resp['code'][0]
                if errcode=='nomgmt':
                    raise BTLEException(BTLEException.MGMT_ERROR, "Management not available (permissions problem?)")
                elif errcode=='comerr':
                    bt_err=resp['bterr'][0]
                    raise BTLEException(BTLEException.COMM_ERROR, "Bluetooth error (%d)" % bt_err, bt_err)
                else:
                    raise BTLEException(BTLEException.INTERNAL_ERROR, "Error (%s)" % errcode)
            elif respType == 'scan':
                # Scan response when we weren't interested. Ignore it
                continue
            else:
                raise BTLEException(BTLEException.INTERNAL_ERROR, "Unexpected response (%s)" % respType)

    def status(self):
        self._writeCmd("stat\n")
        return self._waitResp(['stat'])

    def setSecurityLevel(self, level):
        self._writeCmd("secu %s\n" % level)
        return self._waitResp('stat', 60)

    def unpair(self, addr, addrType):
        assert(isinstance(addr, str))
        assert(isinstance(addrType, str))
        if len(addr.split(":")) != 6:
            raise ValueError("Expected MAC address, got %s" % repr(addr))
        if addrType not in (ADDR_TYPE_PUBLIC, ADDR_TYPE_RANDOM):
            raise ValueError("Expected address type public or random, got {}".format(addrType))
        self._mgmtCmd("unpair %s %s\n" % (addr, addrType))

class Peripheral(BluepyHelper):
    def __init__(self, deviceAddr=None, addrType=ADDR_TYPE_PUBLIC, iface='hci0'):
        BluepyHelper.__init__(self, iface)
        self.services = {} # Indexed by UUID
        self.discoveredAllServices = False
        self.gatts = None
        if isinstance(deviceAddr, ScanEntry):
            addr = deviceAddr.addr
            addr_type = deviceAddr.atype
            self.iface = deviceAddr.iface
        else:
            addr = deviceAddr
            addr_type = addrType
            self.iface = iface

        self._startHelper()
        # Simple connect can work out of the box, but needs reset (and super user perm) in some cases
        self.reset_controller(False)

        if addr is not None:
            self.connect(addr, addr_type)

    def setDelegate(self, delegate_): # same as withDelegate(), deprecated
        return self.withDelegate(delegate_)

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        self.disconnect()

    def _getResp(self, wantType, timeout=None):
        # make sure wantType is a list
        if isinstance(wantType, list) is not True:
            wantType = [wantType]

        while True:
            resp = self._waitResp(wantType + ['ntfy', 'ind', 'gatts'], timeout)
            if resp is None:
                return None

            respType = resp['rsp'][0]
            if respType in ['ntfy', 'ind']:
                hnd = resp['hnd'][0]
                data = resp['d'][0]
                if self.delegate is not None:
                    self.delegate.handleNotification(hnd, data)
                if respType not in wantType:
                    continue
            if respType == 'gatts':
                if self.gatts:
                    data = self.gatts(resp['d'][0])
                    if 23 < len(data):
                        raise Exception
                else:
                    # Send back error not supported
                    data = '\x01' + resp['d'][0][0] + '\x00\x00\x06'

                self._writeCmd("gatts %s\n" % binascii.b2a_hex(data))
                continue

            return resp

    def connect(self, addr, addrType):
        if len(addr.split(":")) != 6:
            raise ValueError("Expected MAC address, got %s" % repr(addr))
        if addrType not in (ADDR_TYPE_PUBLIC, ADDR_TYPE_RANDOM):
            raise ValueError("Expected address type public or random, got {}".format(addrType))
        # Local random addr no supported (see PRIVACY comment)
        self.local_addr = self.local_public
        self.local_type = ADDR_TYPE_PUBLIC
        self.remote_addr = addr
        self.remote_type = addrType
        self._startHelper()
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
        self._getResp('stat', 5)
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
        if 'hstart' not in rsp:
            raise BTLEException(BTLEException.GATT_ERROR, "Service %s not found" % (uuid.getCommonName()))
        svc = Service(self, uuid, rsp['hstart'][0], rsp['hend'][0])
        self.services[uuid] = svc
        return svc

    def _getIncludedServices(self, startHnd=1, endHnd=0xFFFF):
        self._writeCmd("incl %X %X\n" % (startHnd, endHnd))
        rsp = self._getResp('find')
        if 'hstart' not in rsp:
            return None

        return rsp

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

    def readCharacteristic(self, handle, offset=None):
        cmd = "rd %X" % handle
        if offset:
            cmd += " %X" % offset
        self._writeCmd(cmd + "\n")
        resp = self._getResp('rd')
        return resp['d'][0]

    def _readCharacteristicByUUID(self, uuid, startHnd, endHnd):
        # Not used at present
        self._writeCmd("rdu %s %X %X\n" % (UUID(uuid), startHnd, endHnd))
        return self._getResp('rd')

    def writeCharacteristic(self, handle, val, withResponse=False):
        # Without response, a value too long for one packet will be truncated,
        # but with response, it will be sent as a queued write
        cmd = "wrr" if withResponse else "wr"
        self._writeCmd("%s %X %s\n" % (cmd, handle, binascii.b2a_hex(val).decode('utf-8')))
        return self._getResp('wr')

    def setMTU(self, mtu):
        self._writeCmd("mtu %x\n" % mtu)
        return self._getResp('stat')

    def waitForNotifications(self, timeout):
         resp = self._getResp('ntfy', timeout)
         return (resp != None)

    def __del__(self):
        self.disconnect()

class ScanEntry:
    addrTypes = { 1 : ADDR_TYPE_PUBLIC,
                  2 : ADDR_TYPE_RANDOM
                }

    dataTags = {
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

    def __init__(self, addr, iface):
        self.addr = addr
        self.iface = iface
        self.atype = None
        self.rssi = []
        self.connectable = False
        self.rawData = None
        self.scanData = {}
        self.updateCount = 0

    def _update(self, resp):
        atype = self.addrTypes.get(resp['type'][0], None)
        if (self.atype is not None) and (atype != self.atype):
            raise BTLEException("Address type changed during scan, for address %s" % self.addr)
        self.atype = atype
        self.rssi += [ -resp['rssi'][0] ]
        self.connectable = ((resp['flag'][0] & 0x4) == 0)
        data = resp.get('d', [''])[0]
        self.rawData = data

        # Note: bluez is notifying devices twice: once with advertisement data,
        # then with scan response data. Also, the device may update the
        # advertisement or scan data
        isNewData = False
        while len(data) >= 2:
            sdlen, sdid = struct.unpack_from('<BB', data)
            val = data[2 : sdlen + 1]
            if (sdid not in self.scanData) or (val != self.scanData[sdid]):
                isNewData = True
            self.scanData[sdid] = val
            data = data[sdlen + 1:]

        self.updateCount += 1
        return isNewData

    def getDescription(self, sdid):
        return self.dataTags.get(sdid, hex(sdid))

    def getValueText(self, sdid):
        val = self.scanData.get(sdid, None)
        if val is None:
            return None
        if (sdid==8) or (sdid==9):
            return val.decode('utf-8')
        else:
            return binascii.b2a_hex(val).decode('utf-8')

    def getScanData(self):
        '''Returns list of tuples [(tag, description, value)]'''
        return [ (sdid, self.getDescription(sdid), self.getValueText(sdid))
                    for sdid in self.scanData.keys() ]


class Scanner(BluepyHelper):
    def __init__(self, iface='hci0'):
        BluepyHelper.__init__(self, iface)
        self.scanned = {}

    def start(self):
        self._startHelper()
        self.reset_controller(True)
        self._writeCmd("scan\n")
        rsp = self._waitResp("mgmt")
        if rsp["code"][0] == "success":
            return
        # Sometimes previous scan still ongoing
        if rsp["code"][0] == "busy":
            self._mgmtCmd("scanend")
            rsp = self._waitResp("stat")
            assert rsp["state"][0] == "disc"
            self._mgmtCmd("scan")

    def stop(self):
        self._mgmtCmd("scanend")
        self._stopHelper()

    def clear(self):
        self.scanned = {}

    def process(self, timeout=10.0):
        if self._helper is None:
            raise BTLEException(BTLEException.INTERNAL_ERROR,
                                "Helper not started (did you call start()?)")
        start = time.time()
        while True:
            if timeout:
                remain = start + timeout - time.time()
                if remain <= 0.0:
                    break
            else:
                remain = None
            resp = self._waitResp(['scan', 'stat'], remain)
            if resp is None:
                break

            respType = resp['rsp'][0]
            if respType == 'stat':
                # if scan ended, restart it
                if resp['state'][0] == 'disc':
                    self._mgmtCmd("scan")

            elif respType == 'scan':
                # device found
                addr = binascii.b2a_hex(resp['addr'][0]).decode('utf-8')
                addr = ':'.join([addr[i:i+2] for i in range(0,12,2)])
                if addr in self.scanned:
                    dev = self.scanned[addr]
                else:
                    dev = ScanEntry(addr, self._iface)
                    self.scanned[addr] = dev
                isNewData = dev._update(resp)
                if self.delegate :
                    if self.delegate.handleScan(dev, (dev.updateCount <= 1), isNewData):
                        break

            else:
                raise BTLEException(BTLEException.INTERNAL_ERROR, "Unexpected response: " + respType)

    def scan(self, timeout=10):
        self.clear()
        self.start()
        self.process(timeout)
        self.stop()
        return self.scanned.values()

class Central(BluepyHelper):
    def __init__(self, iface='hci0'):
        BluepyHelper.__init__(self, iface)

    def start(self):
        self._startHelper()
        self.reset_controller(True)
        self._mgmtCmd("connectable on")
        self._mgmtCmd("discoverable on")

    def stop(self):
        self._mgmtCmd("discoverable off")
        self._mgmtCmd("advertising off")
        self._stopHelper()

    def advertise(self):
        # Local random addr no supported (see PRIVACY comment)
        self.local_addr = self.local_public
        self.local_type = ADDR_TYPE_PUBLIC

        self._mgmtCmd("advertising on")
        rsp = self._waitResp('stat', 1)
        if rsp['state'][0] != 'advertise':
            raise BTLEException(BTLEException.INTERNAL_ERROR,
                                "Failed to start advertising")
        # Spurious stat sometimes???:
        #self._waitResp(['stat'], 1)

    def connect(self, addr, addrType):
        if len(addr.split(":")) != 6:
            raise ValueError("Expected MAC address, got %s" % repr(addr))
        if addrType not in (ADDR_TYPE_PUBLIC, ADDR_TYPE_RANDOM):
            raise ValueError("Expected address type public or random, got {}".format(addrType))
        self.deviceAddr = addr
        self._writeCmd("conn %s %s\n" % (addr, addrType))
        rsp = self._waitResp('stat')
        while rsp['state'][0] == 'tryconn':
            rsp = self._waitResp('stat')
        if rsp['state'][0] != 'conn':
            self._stopHelper()
            raise BTLEException(BTLEException.DISCONNECTED,
                                "Failed to connect to peripheral %s, addr type: %s" % (addr, addrType))
        return (addr, addrType)

    def wait_conn(self, timeout = None):
        resp = self._waitResp('stat', timeout)
        if resp is None:
            return
        if resp['state'][0] != 'disc':
            raise BTLEException(BTLEException.INTERNAL_ERROR,
                                "Advertising stopped not for a disconnection")
        resp = self._waitResp('mgmt', 1)
        addr = binascii.b2a_hex(resp['addr'][0])
        addr = ':'.join([ addr[2*i:2*i+2] for i in xrange(6)])
        atype = { 1 : 'public', 2 : 'random' }[resp['type'][0]]
        return self.connect(addr, atype)

    def poll(self, timeout = None):
        while True:
            resp = self._waitResp(['stat', 'gatts'], timeout)
            if resp is None:
                break
            respType = resp['rsp'][0]
            if respType == 'stat' and resp['state'][0] == 'disc':
                break
            elif respType == 'gatts':
                if self.gatts:
                    data = self.gatts(resp['d'][0])
                else:
                    # Send back error not supported
                    data = '\x01' + resp['d'][0][0] + '\x00\x00\x06'

                # data can be None : write_cmd, mtu_req
                if data is not None:
                    if 23 < len(data):
                        raise Exception
                    self._writeCmd("gatts %s\n" % binascii.b2a_hex(data))

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

def get_json_uuid():
    import json
    with open(os.path.join(script_path, 'uuids.json'),"rb") as fp:
        uuid_data = json.loads(fp.read().decode("utf-8"))
    for k in ['service_UUIDs', 'characteristic_UUIDs', 'descriptor_UUIDs', 'declaration_UUIDs']:
        for number,cname,name in uuid_data[k]:
            yield UUID(number, cname)
            yield UUID(number, name)

AssignedNumbers = _UUIDNameMap( list(get_json_uuid() ))

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
