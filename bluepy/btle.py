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
import signal
from queue import Queue, Empty
from threading import Thread

def preexec_function():
    # Ignore the SIGINT signal by setting the handler to the standard
    # signal handler SIG_IGN.
    signal.signal(signal.SIGINT, signal.SIG_IGN)

Debugging = False
script_path = os.path.join(os.path.abspath(os.path.dirname(__file__)))
helperExe = os.path.join(script_path, "bluepy-helper")

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
    """Base class for all Bluepy exceptions"""
    def __init__(self, message, resp_dict=None):
        self.message = message

        # optional messages from bluepy-helper
        self.estat = None
        self.emsg = None
        if resp_dict:
            self.estat = resp_dict.get('estat',None)
            if isinstance(self.estat,list):
                self.estat = self.estat[0]
            self.emsg = resp_dict.get('emsg',None)
            if isinstance(self.emsg,list):
                self.emsg = self.emsg[0]


    def __str__(self):
        msg = self.message
        if self.estat or self.emsg:
            msg = msg + " ("
            if self.estat:
                msg = msg + "code: %s" % self.estat
            if self.estat and self.emsg:
                msg = msg + ", "
            if self.emsg:
                msg = msg + "error: %s" % self.emsg
            msg = msg + ")"

        return msg

class BTLEInternalError(BTLEException):
    def __init__(self, message, rsp=None):
        BTLEException.__init__(self, message, rsp)

class BTLEDisconnectError(BTLEException):
    def __init__(self, message, rsp=None):
        BTLEException.__init__(self, message, rsp)

class BTLEManagementError(BTLEException):
    def __init__(self, message, rsp=None):
        BTLEException.__init__(self, message, rsp)

class BTLEGattError(BTLEException):
    def __init__(self, message, rsp=None):
        BTLEException.__init__(self, message, rsp)



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

        self.binVal = binascii.a2b_hex(val.encode('utf-8'))
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
        self.descs = None

    def getCharacteristics(self, forUUID=None):
        if not self.chars: # Unset, or empty
            self.chars = [] if self.hndEnd <= self.hndStart else self.peripheral.getCharacteristics(self.hndStart, self.hndEnd)
        if forUUID is not None:
            u = UUID(forUUID)
            return [ch for ch in self.chars if ch.uuid==u]
        return self.chars

    def getDescriptors(self, forUUID=None):
        if not self.descs:
            # Grab all descriptors in our range, except for the service
            # declaration descriptor
            all_descs = self.peripheral.getDescriptors(self.hndStart+1, self.hndEnd)
            # Filter out the descriptors for the characteristic properties
            # Note that this does not filter out characteristic value descriptors
            self.descs = [desc for desc in all_descs if desc.uuid != 0x2803]
        if forUUID is not None:
            u = UUID(forUUID)
            return [desc for desc in self.descs if desc.uuid == u]
        return self.descs

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
        self.descs = None

    def read(self):
        return self.peripheral.readCharacteristic(self.valHandle)

    def write(self, val, withResponse=False):
        return self.peripheral.writeCharacteristic(self.valHandle, val, withResponse)

    def getDescriptors(self, forUUID=None, hndEnd=0xFFFF):
        if not self.descs:
            # Descriptors (not counting the value descriptor) begin after
            # the handle for the value descriptor and stop when we reach
            # the handle for the next characteristic or service
            self.descs = []
            for desc in self.peripheral.getDescriptors(self.valHandle+1, hndEnd):
                if desc.uuid in (0x2800, 0x2801, 0x2803):
                    # Stop if we reach another characteristic or service
                    break
                self.descs.append(desc)
        if forUUID is not None:
            u = UUID(forUUID)
            return [desc for desc in self.descs if desc.uuid == u]
        return self.descs

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


    def read(self):
        return self.peripheral.readCharacteristic(self.handle)

    def write(self, val, withResponse=False):
        self.peripheral.writeCharacteristic(self.handle, val, withResponse)

class DefaultDelegate:
    def __init__(self):
        pass

    def handleNotification(self, cHandle, data):
        DBG("Notification:", cHandle, "sent data", binascii.b2a_hex(data))

    def handleDiscovery(self, scanEntry, isNewDev, isNewData):
        DBG("Discovered device", scanEntry.addr)

class BluepyHelper:
    def __init__(self):
        self._helper = None
        self._lineq = None
        self._stderr = None
        self._mtu = 0
        self.delegate = DefaultDelegate()

    def withDelegate(self, delegate_):
        self.delegate = delegate_
        return self

    def _startHelper(self,iface=None):
        if self._helper is None:
            DBG("Running ", helperExe)
            self._lineq = Queue()
            self._mtu = 0
            self._stderr = open(os.devnull, "w")
            args=[helperExe]
            if iface is not None: args.append(str(iface))
            self._helper = subprocess.Popen(args,
                                            stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE,
                                            stderr=self._stderr,
                                            universal_newlines=True,
                                            preexec_fn = preexec_function)
            t = Thread(target=self._readToQueue)
            t.daemon = True               # don't wait for it to exit
            t.start()

    def _readToQueue(self):
        """Thread to read lines from stdout and insert in queue."""
        while self._helper:
            line = self._helper.stdout.readline()
            if not line:                  # EOF
                break
            self._lineq.put(line)

    def _stopHelper(self):
        if self._helper is not None:
            DBG("Stopping ", helperExe)
            self._helper.stdin.write("quit\n")
            self._helper.stdin.flush()
            self._helper.wait()
            self._helper = None
        if self._stderr is not None:
            self._stderr.close()
            self._stderr = None

    def _writeCmd(self, cmd):
        if self._helper is None:
            raise BTLEInternalError("Helper not started (did you call connect()?)")
        DBG("Sent: ", cmd)
        self._helper.stdin.write(cmd)
        self._helper.stdin.flush()

    def _mgmtCmd(self, cmd):
        self._writeCmd(cmd + '\n')
        rsp = self._waitResp('mgmt')
        if rsp['code'][0] != 'success':
            self._stopHelper()
            raise BTLEManagementError("Failed to execute management command '%s'" % (cmd), rsp)

    @staticmethod
    def parseResp(line):
        resp = {}
        for item in line.rstrip().split('\x1e'):
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
                raise BTLEInternalError("Cannot understand response value %s" % repr(tval))
            if tag not in resp:
                resp[tag] = [val]
            else:
                resp[tag].append(val)
        return resp

    def _waitResp(self, wantType, timeout=None):
        while True:
            if self._helper.poll() is not None:
                raise BTLEInternalError("Helper exited")

            try:
                rv = self._lineq.get(timeout=timeout)
            except Empty:
                DBG("Select timeout")
                return None

            DBG("Got:", repr(rv))
            if rv.startswith('#') or rv == '\n' or len(rv)==0:
                continue

            resp = BluepyHelper.parseResp(rv)
            if 'rsp' not in resp:
                raise BTLEInternalError("No response type indicator", resp)

            respType = resp['rsp'][0]

            # always check for MTU updates
            if 'mtu' in resp and len(resp['mtu']) > 0:
                new_mtu = int(resp['mtu'][0])
                if self._mtu != new_mtu:
                    self._mtu = new_mtu
                    DBG("Updated MTU: " + str(self._mtu))

            if respType in wantType:
                return resp
            elif respType == 'stat':
                if 'state' in resp and len(resp['state']) > 0 and resp['state'][0] == 'disc':
                    self._stopHelper()
                    raise BTLEDisconnectError("Device disconnected", resp)
            elif respType == 'err':
                errcode=resp['code'][0]
                if errcode=='nomgmt':
                    raise BTLEManagementError("Management not available (permissions problem?)", resp)
                elif errcode=='atterr':
                    raise BTLEGattError("Bluetooth command failed", resp)
                else:
                    raise BTLEException("Error from bluepy-helper (%s)" % errcode, resp)
            elif respType == 'scan':
                # Scan response when we weren't interested. Ignore it
                continue
            else:
                raise BTLEInternalError("Unexpected response (%s)" % respType, resp)

    def status(self):
        self._writeCmd("stat\n")
        return self._waitResp(['stat'])


class Peripheral(BluepyHelper):
    def __init__(self, deviceAddr=None, addrType=ADDR_TYPE_PUBLIC, iface=None, timeout=None):
        BluepyHelper.__init__(self)
        self._serviceMap = None # Indexed by UUID
        (self.deviceAddr, self.addrType, self.iface) = (None, None, None)

        if isinstance(deviceAddr, ScanEntry):
            self._connect(deviceAddr.addr, deviceAddr.addrType, deviceAddr.iface, timeout)
        elif deviceAddr is not None:
            self._connect(deviceAddr, addrType, iface, timeout)

    def setDelegate(self, delegate_): # same as withDelegate(), deprecated
        return self.withDelegate(delegate_)

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        self.disconnect()

    def _getResp(self, wantType, timeout=None):
        if isinstance(wantType, list) is not True:
            wantType = [wantType]

        while True:
            resp = self._waitResp(wantType + ['ntfy', 'ind'], timeout)
            if resp is None:
                return None

            respType = resp['rsp'][0]
            if respType == 'ntfy' or respType == 'ind':
                hnd = resp['hnd'][0]
                data = resp['d'][0]
                if self.delegate is not None:
                    self.delegate.handleNotification(hnd, data)
            if respType not in wantType:
                continue
            return resp

    def _connect(self, addr, addrType=ADDR_TYPE_PUBLIC, iface=None, timeout=None):
        if len(addr.split(":")) != 6:
            raise ValueError("Expected MAC address, got %s" % repr(addr))
        if addrType not in (ADDR_TYPE_PUBLIC, ADDR_TYPE_RANDOM):
            raise ValueError("Expected address type public or random, got {}".format(addrType))
        self._startHelper(iface)
        self.addr = addr
        self.addrType = addrType
        self.iface = iface
        if iface is not None:
            self._writeCmd("conn %s %s %s\n" % (addr, addrType, "hci"+str(iface)))
        else:
            self._writeCmd("conn %s %s\n" % (addr, addrType))
        rsp = self._getResp('stat', timeout)
        timeout_exception = BTLEDisconnectError(
            "Timed out while trying to connect to peripheral %s, addr type: %s" %
            (addr, addrType), rsp)
        if rsp is None:
            raise timeout_exception
        while rsp and rsp['state'][0] == 'tryconn':
            rsp = self._getResp('stat', timeout)
        if rsp is None or rsp['state'][0] != 'conn':
            self._stopHelper()
            if rsp is None:
                raise timeout_exception
            else:
                raise BTLEDisconnectError("Failed to connect to peripheral %s, addr type: %s"
                                          % (addr, addrType), rsp)

    def connect(self, addr, addrType=ADDR_TYPE_PUBLIC, iface=None, timeout=None):
        if isinstance(addr, ScanEntry):
            self._connect(addr.addr, addr.addrType, addr.iface, timeout)
        elif addr is not None:
            self._connect(addr, addrType, iface, timeout)

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
        self._serviceMap = {}
        for i in range(nSvcs):
            self._serviceMap[UUID(uuids[i])] = Service(self, uuids[i], starts[i], ends[i])
        return self._serviceMap

    def getState(self):
        status = self.status()
        return status['state'][0]

    @property
    def services(self):
        if self._serviceMap is None:
            self._serviceMap = self.discoverServices()
        return self._serviceMap.values()

    def getServices(self):
        return self.services

    def getServiceByUUID(self, uuidVal):
        uuid = UUID(uuidVal)
        if self._serviceMap is not None and uuid in self._serviceMap:
            return self._serviceMap[uuid]
        self._writeCmd("svcs %s\n" % uuid)
        rsp = self._getResp('find')
        if 'hstart' not in rsp:
            raise BTLEGattError("Service %s not found" % (uuid.getCommonName()), rsp)
        svc = Service(self, uuid, rsp['hstart'][0], rsp['hend'][0])

        if self._serviceMap is None:
            self._serviceMap = {}
        self._serviceMap[uuid] = svc
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
        # Historical note:
        # Certain Bluetooth LE devices are not capable of sending back all
        # descriptors in one packet due to the limited size of MTU. So the
        # guest needs to check the response and make retries until all handles
        # are returned.
        # In bluez 5.25 and later, gatt_discover_desc() in attrib/gatt.c does the retry
        # so bluetooth_helper always returns a full list.
        # This was broken in earlier versions.
        resp = self._getResp('desc')
        ndesc = len(resp['hnd'])
        return [Descriptor(self, resp['uuid'][i], resp['hnd'][i]) for i in range(ndesc)]

    def readCharacteristic(self, handle):
        self._writeCmd("rd %X\n" % handle)
        resp = self._getResp('rd')
        return resp['d'][0]

    def _readCharacteristicByUUID(self, uuid, startHnd, endHnd):
        # Not used at present
        self._writeCmd("rdu %s %X %X\n" % (UUID(uuid), startHnd, endHnd))
        return self._getResp('rd')

    def writeCharacteristic(self, handle, val, withResponse=False, timeout=None):
        # Without response, a value too long for one packet will be truncated,
        # but with response, it will be sent as a queued write
        cmd = "wrr" if withResponse else "wr"
        self._writeCmd("%s %X %s\n" % (cmd, handle, binascii.b2a_hex(val).decode('utf-8')))
        return self._getResp('wr', timeout)

    def setSecurityLevel(self, level):
        self._writeCmd("secu %s\n" % level)
        return self._getResp('stat')

    def unpair(self):
        self._mgmtCmd("unpair")

    def pair(self):
        self._mgmtCmd("pair")

    def getMTU(self):
        return self._mtu

    def setMTU(self, mtu):
        self._writeCmd("mtu %x\n" % mtu)
        return self._getResp('stat')

    def waitForNotifications(self, timeout):
         resp = self._getResp(['ntfy','ind'], timeout)
         return (resp != None)

    def _setRemoteOOB(self, address, address_type, oob_data, iface=None):
        if self._helper is None:
            self._startHelper(iface)
        self.addr = address
        self.addrType = address_type
        self.iface = iface
        cmd = "remote_oob " + address + " " + address_type
        if oob_data['C_192'] is not None and oob_data['R_192'] is not None:
            cmd += " C_192 " + oob_data['C_192'] + " R_192 " + oob_data['R_192']
        if oob_data['C_256'] is not None and oob_data['R_256'] is not None:
            cmd += " C_256 " + oob_data['C_256'] + " R_256 " + oob_data['R_256']
        if iface is not None:
            cmd += " hci"+str(iface)
        self._writeCmd(cmd)

    def setRemoteOOB(self, address, address_type, oob_data, iface=None):
        if len(address.split(":")) != 6:
            raise ValueError("Expected MAC address, got %s" % repr(address))
        if address_type not in (ADDR_TYPE_PUBLIC, ADDR_TYPE_RANDOM):
            raise ValueError("Expected address type public or random, got {}".format(address_type))
        if isinstance(address, ScanEntry):
            return self._setOOB(address.addr, address.addrType, oob_data, address.iface)
        elif address is not None:
            return self._setRemoteOOB(address, address_type, oob_data, iface)

    def getLocalOOB(self, iface=None):
        if self._helper is None:
            self._startHelper(iface)
        self.iface = iface
        self._writeCmd("local_oob\n")
        if iface is not None:
            cmd += " hci"+str(iface)
        resp = self._getResp('oob')
        if resp is not None:
            data = resp.get('d', [''])[0]
            if data is None:
                raise BTLEManagementError(
                                "Failed to get local OOB data.")
            if struct.unpack_from('<B',data,0)[0] != 8 or struct.unpack_from('<B',data,1)[0] != 0x1b:
                raise BTLEManagementError(
                                "Malformed local OOB data (address).")
            address = data[2:8]
            address_type = data[8:9]
            if struct.unpack_from('<B',data,9)[0] != 2 or struct.unpack_from('<B',data,10)[0] != 0x1c:
                raise BTLEManagementError(
                                "Malformed local OOB data (role).")
            role = data[11:12]
            if struct.unpack_from('<B',data,12)[0] != 17 or struct.unpack_from('<B',data,13)[0] != 0x22:
                raise BTLEManagementError(
                                "Malformed local OOB data (confirm).")
            confirm = data[14:30]
            if struct.unpack_from('<B',data,30)[0] != 17 or struct.unpack_from('<B',data,31)[0] != 0x23:
                raise BTLEManagementError(
                                "Malformed local OOB data (random).")
            random = data[32:48]
            if struct.unpack_from('<B',data,48)[0] != 2 or struct.unpack_from('<B',data,49)[0] != 0x1:
                raise BTLEManagementError(
                                "Malformed local OOB data (flags).")
            flags = data[50:51]
            return {'Address' : ''.join(["%02X" % struct.unpack('<B',c)[0] for c in address]),
                    'Type' : ''.join(["%02X" % struct.unpack('<B',c)[0] for c in address_type]),
                    'Role' : ''.join(["%02X" % struct.unpack('<B',c)[0] for c in role]),
                    'C_256' : ''.join(["%02X" % struct.unpack('<B',c)[0] for c in confirm]),
                    'R_256' : ''.join(["%02X" % struct.unpack('<B',c)[0] for c in random]),
                    'Flags' : ''.join(["%02X" % struct.unpack('<B',c)[0] for c in flags]),
                    }

    def __del__(self):
        self.disconnect()

class ScanEntry:
    addrTypes = { 1 : ADDR_TYPE_PUBLIC,
                  2 : ADDR_TYPE_RANDOM
                }

    FLAGS                     = 0x01
    INCOMPLETE_16B_SERVICES   = 0x02
    COMPLETE_16B_SERVICES     = 0x03
    INCOMPLETE_32B_SERVICES   = 0x04
    COMPLETE_32B_SERVICES     = 0x05
    INCOMPLETE_128B_SERVICES  = 0x06
    COMPLETE_128B_SERVICES    = 0x07
    SHORT_LOCAL_NAME          = 0x08
    COMPLETE_LOCAL_NAME       = 0x09
    TX_POWER                  = 0x0A
    SERVICE_SOLICITATION_16B  = 0x14
    SERVICE_SOLICITATION_32B  = 0x1F
    SERVICE_SOLICITATION_128B = 0x15
    SERVICE_DATA_16B          = 0x16
    SERVICE_DATA_32B          = 0x20
    SERVICE_DATA_128B         = 0x21
    PUBLIC_TARGET_ADDRESS     = 0x17
    RANDOM_TARGET_ADDRESS     = 0x18
    APPEARANCE                = 0x19
    ADVERTISING_INTERVAL      = 0x1A
    MANUFACTURER              = 0xFF

    dataTags = {
        FLAGS                     : 'Flags',
        INCOMPLETE_16B_SERVICES   : 'Incomplete 16b Services',
        COMPLETE_16B_SERVICES     : 'Complete 16b Services',
        INCOMPLETE_32B_SERVICES   : 'Incomplete 32b Services',
        COMPLETE_32B_SERVICES     : 'Complete 32b Services',
        INCOMPLETE_128B_SERVICES  : 'Incomplete 128b Services',
        COMPLETE_128B_SERVICES    : 'Complete 128b Services',
        SHORT_LOCAL_NAME          : 'Short Local Name',
        COMPLETE_LOCAL_NAME       : 'Complete Local Name',
        TX_POWER                  : 'Tx Power',
        SERVICE_SOLICITATION_16B  : '16b Service Solicitation',
        SERVICE_SOLICITATION_32B  : '32b Service Solicitation',
        SERVICE_SOLICITATION_128B : '128b Service Solicitation',
        SERVICE_DATA_16B          : '16b Service Data',
        SERVICE_DATA_32B          : '32b Service Data',
        SERVICE_DATA_128B         : '128b Service Data',
        PUBLIC_TARGET_ADDRESS     : 'Public Target Address',
        RANDOM_TARGET_ADDRESS     : 'Random Target Address',
        APPEARANCE                : 'Appearance',
        ADVERTISING_INTERVAL      : 'Advertising Interval',
        MANUFACTURER              : 'Manufacturer',
    }

    def __init__(self, addr, iface):
        self.addr = addr
        self.iface = iface
        self.addrType = None
        self.rssi = None
        self.connectable = False
        self.rawData = None
        self.scanData = {}
        self.updateCount = 0

    def _update(self, resp):
        addrType = self.addrTypes.get(resp['type'][0], None)
        if (self.addrType is not None) and (addrType != self.addrType):
            raise BTLEInternalError("Address type changed during scan, for address %s" % self.addr)
        self.addrType = addrType
        self.rssi = -resp['rssi'][0]
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

    def _decodeUUID(self, val, nbytes):
        if len(val) < nbytes:
            return None
        bval=bytearray(val)
        rs=""
        # Bytes are little-endian; convert to big-endian string
        for i in range(nbytes):
            rs = ("%02X" % bval[i]) + rs
        return UUID(rs)

    def _decodeUUIDlist(self, val, nbytes):
        result = []
        for i in range(0, len(val), nbytes):
            if len(val) >= (i+nbytes):
                result.append(self._decodeUUID(val[i:i+nbytes],nbytes))
        return result

    def getDescription(self, sdid):
        return self.dataTags.get(sdid, hex(sdid))

    def getValue(self, sdid):
        val = self.scanData.get(sdid, None)
        if val is None:
            return None
        if sdid in [ScanEntry.SHORT_LOCAL_NAME, ScanEntry.COMPLETE_LOCAL_NAME]:
            try:
                # Beware! Vol 3 Part C 18.3 doesn't give an encoding. Other references
                # to 'local name' (e.g. vol 3 E, 6.23) suggest it's UTF-8 but in practice
                # devices sometimes have garbage here. See #259, #275, #292.
                return val.decode('utf-8')
            except UnicodeDecodeError:
                bbval = bytearray(val)
                return ''.join( [ (chr(x) if (x>=32 and x<=127) else '?') for x in bbval ] )
        elif sdid in [ScanEntry.INCOMPLETE_16B_SERVICES, ScanEntry.COMPLETE_16B_SERVICES]:
            return self._decodeUUIDlist(val,2)
        elif sdid in [ScanEntry.INCOMPLETE_32B_SERVICES, ScanEntry.COMPLETE_32B_SERVICES]:
            return self._decodeUUIDlist(val,4)
        elif sdid in [ScanEntry.INCOMPLETE_128B_SERVICES, ScanEntry.COMPLETE_128B_SERVICES]:
            return self._decodeUUIDlist(val,16)
        else:
            return val

    def getValueText(self, sdid):
        val = self.getValue(sdid)
        if val is None:
            return None
        if sdid in [ScanEntry.SHORT_LOCAL_NAME, ScanEntry.COMPLETE_LOCAL_NAME]:
            return val
        elif isinstance(val, list):
            return ','.join(str(v) for v in val)
        else:
            return binascii.b2a_hex(val).decode('ascii')

    def getScanData(self):
        '''Returns list of tuples [(tag, description, value)]'''
        return [ (sdid, self.getDescription(sdid), self.getValueText(sdid))
                    for sdid in self.scanData.keys() ]


class Scanner(BluepyHelper):
    def __init__(self,iface=0):
        BluepyHelper.__init__(self)
        self.scanned = {}
        self.iface=iface
        self.passive=False

    def _cmd(self):
        return "pasv" if self.passive else "scan"

    def start(self, passive=False):
        self.passive = passive
        self._startHelper(iface=self.iface)
        self._mgmtCmd("le on")
        self._writeCmd(self._cmd()+"\n")
        rsp = self._waitResp("mgmt")
        if rsp["code"][0] == "success":
            return
        # Sometimes previous scan still ongoing
        if rsp["code"][0] == "busy":
            self._mgmtCmd(self._cmd()+"end")
            rsp = self._waitResp("stat")
            assert rsp["state"][0] == "disc"
            self._mgmtCmd(self._cmd())

    def stop(self):
        self._mgmtCmd(self._cmd()+"end")
        self._stopHelper()

    def clear(self):
        self.scanned = {}

    def process(self, timeout=10.0):
        if self._helper is None:
            raise BTLEInternalError(
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
                    self._mgmtCmd(self._cmd())

            elif respType == 'scan':
                # device found
                addr = binascii.b2a_hex(resp['addr'][0]).decode('utf-8')
                addr = ':'.join([addr[i:i+2] for i in range(0,12,2)])
                if addr in self.scanned:
                    dev = self.scanned[addr]
                else:
                    dev = ScanEntry(addr, self.iface)
                    self.scanned[addr] = dev
                isNewData = dev._update(resp)
                if self.delegate is not None:
                    self.delegate.handleDiscovery(dev, (dev.updateCount <= 1), isNewData)

            else:
                raise BTLEInternalError("Unexpected response: " + respType, resp)

    def getDevices(self):
        return self.scanned.values()

    def scan(self, timeout=10, passive=False):
        self.clear()
        self.start(passive=passive)
        self.process(timeout)
        self.stop()
        return self.getDevices()


def capitaliseName(descr):
    words = descr.replace("("," ").replace(")"," ").replace('-',' ').split(" ")
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
    for k in uuid_data.keys():
        for number,cname,name in uuid_data[k]:
            yield UUID(number, cname)
            yield UUID(number, name)

AssignedNumbers = _UUIDNameMap( get_json_uuid() )

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
        for svc in conn.services:
            print(str(svc), ":")
            for ch in svc.getCharacteristics():
                print("    {}, hnd={}, supports {}".format(ch, hex(ch.handle), ch.propertiesToString()))
                chName = AssignedNumbers.getCommonName(ch.uuid)
                if (ch.supportsRead()):
                    try:
                        print("    ->", repr(ch.read()))
                    except BTLEException as e:
                        print("    ->", e)

    finally:
        conn.disconnect()
