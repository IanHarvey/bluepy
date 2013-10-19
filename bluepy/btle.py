
import sys, os, time
import subprocess
import binascii

helperExe = os.path.join(os.path.abspath(os.path.dirname(__file__)), "bluepy-helper")
if not os.path.isfile(helperExe):
    raise ImportError("Cannot find required executable '%s'" % helperExe)

SEC_LEVEL_LOW    = "low"
SEC_LEVEL_MEDIUM = "medium"
SEC_LEVEL_HIGH   = "high"

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
        print "Running", helperExe
        self._helper = subprocess.Popen([helperExe],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self._cmdFd = self._helper.stdin
        self._respFd = self._helper.stdout

        if deviceAddr != None:
            self.connect(deviceAddr)
        self.services = {} # Indexed by UUID
        self.discoveredAllServices = False

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
                raise ValueError("Cannot understand response value %s" % repr(tval))
            if tag not in resp:
                resp[tag] = [val]
            else:
                resp[tag].append(val)
        return resp
        # Umm, stuff here ...!

    def _getResp(self):
	while True:
            rv = self._respFd.readline()
            #print "Got:", repr(rv)
            if not rv.startswith('#'):
                return Peripheral.parseResp(rv)

    def status(self):
	self._cmdFd.write("stat\n")
        return self._getResp()

    def connect(self,addr):
        if len( addr.split(":") ) != 6:
            raise ValueError("Expected MAC address, got %s", repr(addr))
        self.deviceAddr = addr
	self._cmdFd.write("conn %s\n" % addr)
        rsp = self._getResp()
        while rsp['state'][0] == 'tryconn':
            rsp = self._getResp()
        print "Response", rsp
        # TODO handle errors

    def disconnect(self):
	self._cmdFd.write("disc\n")
        return self._getResp()

    def discoverServices(self):
	self._cmdFd.write("svcs\n")
        rsp = self._getResp()
        assert( rsp['rsp'][0]=='find' )
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
	self._cmdFd.write("svcs %s\n" % uuid)
        rsp = self._getResp()
        assert( rsp['rsp'][0]=='find' )
        svc = Service(self, uuid, rsp['hstart'][0], rsp['hend'][0])
        self.services[uuid] = svc
        return svc

    def _getIncludedServices(self,startHnd=1,endHnd=0xFFFF):
        # TODO: No working example of this yet
        self._cmdFd.write("incl %X %X\n" % (startHnd, endHnd) )
        return self._getResp()

    def getCharacteristics(self,startHnd=1,endHnd=0xFFFF, uuid=None):
        cmd = 'char %X %X' % (startHnd, endHnd)
        if uuid:
            cmd += ' '+str(UUID(uuid))
        self._cmdFd.write(cmd + "\n")
        rsp = self._getResp()
        assert( rsp['rsp'][0]=='find' )
        nChars = len(rsp['hnd'])
        return [ Characteristic(self, rsp['uuid'][i], rsp['hnd'][i],
                                rsp['props'][i], rsp['vhnd'][i]) for i in range(nChars) ]

    def getDescriptors(self,startHnd=1,endHnd=0xFFFF):
        self._cmdFd.write("desc %X %X\n" % (startHnd, endHnd) )
        resp = self._getResp()
        assert(resp['rsp'][0]=='desc')
        nDesc = len(resp['hnd'])
        return [ Descriptor(self, resp['uuid'][i], resp['hnd'][i]) for i in range(nDesc) ]

    def readCharacteristic(self,handle):
        self._cmdFd.write("rd %X\n" % handle)
        resp = self._getResp()
        assert(resp['rsp'][0]=='rd')
        return resp['d'][0]

    def _readCharacteristicByUUID(self,uuid,startHnd,endHnd):
        # Not used at present
        self._cmdFd.write("rdu %s %X %X\n" % (str(UUID(uuid)), startHnd, endHnd) )
        return self._getResp()

    def writeCharacteristic(self,handle,val,withResponse=False):
        cmd="wrr" if withResponse else "wr";
        self._cmdFd.write("%s %X %s\n" % (cmd, handle, binascii.b2a_hex(val)) )
        return self._getResp()

    def setSecurityLevel(self,level):
	self._cmdFd.write("secu %s\n" % level)
        return self._getResp()

    def setMTU(self,mtu):
        self._cmdFd.write("mtu %x\n" % mtu)
        return self._getResp()
  
    def __del__(self):
        if self._helper != None:
            self._helper.stdin.write("quit\n")
            self._helper.wait()
            self._helper = None

def strList(l, indent="  "):
    sep = ",\n" + indent
    return indent + (sep.join([ str(i) for i in l ]))

if __name__ == '__main__':
    print "Simple test"
    print UUID(0x270A)
    print UUID("f000aa11-0451-4000-b000-000000000000")
    print UUID("f000aa1204514000b000000000000000")

    conn = Peripheral("BC:6A:29:AB:D3:7A")
    try:
        for svc in conn.getServices():
            print str(svc), ": ----"
            print strList(svc.getCharacteristics())
        svc = conn.getServiceByUUID("f000aa10-0451-4000-b000-000000000000") # Accelerometer
        config = svc.getCharacteristics("f000aa12-0451-4000-b000-000000000000")[0]
        config.write("\x01") # Enable
        accel = svc.getCharacteristics("f000aa11-0451-4000-b000-000000000000")[0]
        for i in range(10):
            raw = accel.read()
            (x,y,z) = [ ((ord(db) ^ 0x80) - 0x80)/64.0 for db in raw ]
            print "X=%.2f Y=%.2f Z=%.2f" % (x,y,z)
    finally:
        conn.disconnect()



