
import sys, os, time
import subprocess
import binascii

Debugging = True

helperExe = os.path.join(os.path.abspath(os.path.dirname(__file__)), "bluepy-helper")
if not os.path.isfile(helperExe):
    raise ImportError("Cannot find required executable '%s'" % helperExe)

SEC_LEVEL_LOW    = "low"
SEC_LEVEL_MEDIUM = "medium"
SEC_LEVEL_HIGH   = "high"

def DBG(*args):
    if Debugging:
        msg = " ".join([str(a) for a in args])
        print (msg)
 
class BTLEException(Exception):
    DISCONNECTED = 1
    COMM_ERROR = 2
    INTERNAL_ERROR = 3
    
    def __init__(self, code, message):
        self.code = code
        self.message = message

    def __str__(self):
        return self.message


class UUID:
    def __init__(self, val):
        '''We accept: 32-digit hex strings, with and without '-' characters,
           4 to 8 digit hex strings, and integers''' 
        if isinstance(val,int) or isinstance(val,long):
            if (val < 0) or (val > 0xFFFFFFFF):
                raise ValueError("Short form UUIDs must be in range 0..0xFFFFFFFF")
            val = "%04X" % val
        else:
            val = str(val) # Do our best

        val = val.replace("-","") 
        if len(val) <= 8: # Short form 
            val = ("0" * (8-len(val))) + val +"00001000800000805F9B34FB"

        self.binVal = binascii.a2b_hex(val) 
        if len(self.binVal) != 16:
            raise ValueError("UUID must be 16 bytes, got '%s'" % val)

    def __str__(self):
        s = binascii.b2a_hex(self.binVal)
        return "-".join([ s[0:8], s[8:12], s[12:16], s[16:20], s[20:32] ])

    def __cmp__(self, other):
        return cmp(str(self), str(other))

    def __hash__(self):
        return hash(str(self))

    def friendlyName(self):
        #TODO
        return str(self)

class Service:
    def __init__(self, *args):
        (self.peripheral, uuidVal, self.hndStart, self.hndEnd) = args
        self.uuid = UUID(uuidVal)
        self.chars = None

    def getCharacteristics(self, forUUID=None):
        if not self.chars: # Unset, or empty
            self.chars = self.peripheral.getCharacteristics(self.hndStart, self.hndEnd)
        if forUUID != None:
            u = UUID(forUUID)
            return [ ch for ch in self.chars if ch.uuid==u ]
        return self.chars

    def __str__(self):
        return "Service <%s>" % str(self.uuid)

class Characteristic:
    def __init__(self, *args):
        (self.peripheral, uuidVal, self.handle, self.properties, self.valHandle) = args
        self.uuid = UUID(uuidVal)

    def read(self):
        return self.peripheral.readCharacteristic(self.valHandle)

    def write(self,val,withResponse=False):
        self.peripheral.writeCharacteristic(self.valHandle,val,withResponse)

    # TODO: descriptors 

    def __str__(self):
        return "Characteristic <%s>" % (self.uuid)

class Descriptor:
    def __init__(self, *args):
        (self.peripheral, uuidVal, self.handle) = args
        self.uuid = UUID(uuidVal)

    def __str__(self):
        return "Descriptor <%s>" % str(self.uuid)

class Peripheral:
    def __init__(self, deviceAddr=None):
        self._helper = None
        self.services = {} # Indexed by UUID
        self.discoveredAllServices = False
        if deviceAddr != None:
            self.connect(deviceAddr)

    def _startHelper(self):
        if self._helper == None:
            DBG("Running ", helperExe)
            self._helper = subprocess.Popen([helperExe],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def _stopHelper(self):
        if self._helper != None:
            DBG("Stopping ", helperExe)
            self._helper.stdin.write("quit\n")
            self._helper.wait()
            self._helper = None

    def _writeCmd(self, cmd):
        if self._helper == None:
            raise BTLEException(BTLEException.INTERNAL_ERROR, "Helper not started (did you call connect()?)")
        DBG("Sent: ", cmd)
        self._helper.stdin.write(cmd)

    @staticmethod
    def parseResp(line):
        resp = {}
        for item in line.rstrip().split(' '):
            (tag,tval) = item.split('=')
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
            self._helper.poll()
            if self._helper.returncode != None:
                raise BTLEException(BTLEException.INTERNAL_ERROR, "Helper exited")
      
            rv = self._helper.stdout.readline()
            DBG("Got:", repr(rv))
            if not rv.startswith('#'):
                resp = Peripheral.parseResp(rv)
                break
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
        else:
            raise BTLEException(BTLEException.INTERNAL_ERROR, "Unexpected response (%s)" % respType)

    def status(self):
	self._writeCmd("stat\n")
        return self._getResp('stat')

    def connect(self,addr):
        if len( addr.split(":") ) != 6:
            raise ValueError("Expected MAC address, got %s", repr(addr))
        self._startHelper()
        self.deviceAddr = addr
	self._writeCmd("conn %s\n" % addr)
        rsp = self._getResp('stat')
        while rsp['state'][0] == 'tryconn':
            rsp = self._getResp('stat')
        if rsp['state'][0] != 'conn':
            self._stopHelper()
            raise BTLEException(BTLEException.DISCONNECTED, "Failed to connect to peripheral")

    def disconnect(self):
        if self._helper==None:
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
        assert( len(starts)==nSvcs and len(ends)==nSvcs )
        self.services = {}
        for i in range(nSvcs):
            self.services[uuids[i]] = Service(self, uuids[i], starts[i], ends[i])
        self.discoveredAllServices = True
        return self.services
        
    def getServices(self):
        if not self.discoveredAllServices:
            self.discoverServices()
        return self.services.values()

    def getServiceByUUID(self,uuidVal):
        uuid=UUID(uuidVal)
        if uuid in self.services:
            return self.services[uuid]
	self._writeCmd("svcs %s\n" % uuid)
        rsp = self._getResp('find')
        svc = Service(self, uuid, rsp['hstart'][0], rsp['hend'][0])
        self.services[uuid] = svc
        return svc

    def _getIncludedServices(self,startHnd=1,endHnd=0xFFFF):
        # TODO: No working example of this yet
        self._writeCmd("incl %X %X\n" % (startHnd, endHnd) )
        return self._getResp('find')

    def getCharacteristics(self,startHnd=1,endHnd=0xFFFF, uuid=None):
        cmd = 'char %X %X' % (startHnd, endHnd)
        if uuid:
            cmd += ' '+str(UUID(uuid))
        self._writeCmd(cmd + "\n")
        rsp = self._getResp('find')
        nChars = len(rsp['hnd'])
        return [ Characteristic(self, rsp['uuid'][i], rsp['hnd'][i],
                                rsp['props'][i], rsp['vhnd'][i]) for i in range(nChars) ]

    def getDescriptors(self,startHnd=1,endHnd=0xFFFF):
        self._writeCmd("desc %X %X\n" % (startHnd, endHnd) )
        resp = self._getResp('desc')
        nDesc = len(resp['hnd'])
        return [ Descriptor(self, resp['uuid'][i], resp['hnd'][i]) for i in range(nDesc) ]

    def readCharacteristic(self,handle):
        self._writeCmd("rd %X\n" % handle)
        resp = self._getResp('rd')
        return resp['d'][0]

    def _readCharacteristicByUUID(self,uuid,startHnd,endHnd):
        # Not used at present
        self._writeCmd("rdu %s %X %X\n" % (str(UUID(uuid)), startHnd, endHnd) )
        return self._getResp('rd')

    def writeCharacteristic(self,handle,val,withResponse=False):
        cmd="wrr" if withResponse else "wr";
        self._writeCmd("%s %X %s\n" % (cmd, handle, binascii.b2a_hex(val)) )
        return self._getResp('wr')

    def setSecurityLevel(self,level):
	self._writeCmd("secu %s\n" % level)
        return self._getResp('stat')

    def setMTU(self,mtu):
        self._writeCmd("mtu %x\n" % mtu)
        return self._getResp('stat')
  
    def __del__(self):
        self.disconnect()

class AssignedNumbers:
    # TODO: full list
    deviceName   = UUID("2A00")
    txPowerLevel = UUID("2A07")
    batteryLevel = UUID("2A19")
    modelNumberString = UUID("2A24")
    serialNumberString = UUID("2A25")
    firmwareRevisionString = UUID("2A26")
    hardwareRevisionString = UUID("2A27")
    softwareRevisionString = UUID("2A28")
    manufacturerNameString = UUID("2A29")
    
    nameMap = { 
    	deviceName : "Device Name",
        txPowerLevel : "Tx Power Level",
        batteryLevel : "Battery Level", 
        modelNumberString : "Model Number String",
        serialNumberString : "Serial Number String",
        firmwareRevisionString : "Firmware Revision String",
        hardwareRevisionString : "Hardware Revision String",
        softwareRevisionString : "Software Revision String",
        manufacturerNameString : "Manufacturer Name String",
    }
    
    @staticmethod
    def getCommonName(uuid):
        return AssignedNumbers.nameMap.get(uuid, None)
        
if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit("Usage:\n  %s <mac-address>" % sys.argv[0])

    Debugging = False
    devaddr = sys.argv[1]
    print "Connecting to:", devaddr
    conn = Peripheral(devaddr)
    try:
        for svc in conn.getServices():
            print str(svc), ":"
            for ch in svc.getCharacteristics():
                print "    " + str(ch)
                chName = AssignedNumbers.getCommonName(ch.uuid)
                if chName != None:
                    print "    ->", chName, repr(ch.read())
    finally:
        conn.disconnect()



