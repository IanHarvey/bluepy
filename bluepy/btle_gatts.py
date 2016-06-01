import struct
import binascii
import btle
from asyncore import read

ATT_OP_ERROR                = '\x01'
ATT_OP_MTU_REQ              = '\x02'
ATT_OP_MTU_RESP             = '\x03'
ATT_OP_FIND_INFO_REQ        = '\x04'
ATT_OP_FIND_INFO_RESP       = '\x05'
ATT_OP_FIND_BY_TYPE_REQ     = '\x06'
ATT_OP_FIND_BY_TYPE_RESP    = '\x07'
ATT_OP_READ_BY_TYPE_REQ     = '\x08'
ATT_OP_READ_BY_TYPE_RESP    = '\x09'
ATT_OP_READ_REQ             = '\x0A'
ATT_OP_READ_RESP            = '\x0B'
ATT_OP_READ_BLOB_REQ        = '\x0C'
ATT_OP_READ_BLOB_RESP       = '\x0D'
ATT_OP_READ_MULTI_REQ       = '\x0E'
ATT_OP_READ_MULTI_RESP      = '\x0F'
ATT_OP_READ_BY_GROUP_REQ    = '\x10'
ATT_OP_READ_BY_GROUP_RESP   = '\x11'
ATT_OP_WRITE_REQ            = '\x12'
ATT_OP_WRITE_RESP           = '\x13'
ATT_OP_WRITE_CMD            = '\x52'
ATT_OP_PREP_WRITE_REQ       = '\x16'
ATT_OP_PREP_WRITE_RESP      = '\x17'
ATT_OP_EXEC_WRITE_REQ       = '\x18'
ATT_OP_EXEC_WRITE_RESP      = '\x19'
ATT_OP_HANDLE_NOTIFY        = '\x1B'
ATT_OP_HANDLE_IND           = '\x1D'
ATT_OP_HANDLE_CNF           = '\x1E'
ATT_OP_SIGNED_WRITE_CMD     = '\xD2'

ATT_ECODE_SUCCESS               = '\x00'
ATT_ECODE_INVALID_HANDLE        = '\x01'
ATT_ECODE_READ_NOT_PERM         = '\x02'
ATT_ECODE_WRITE_NOT_PERM        = '\x03'
ATT_ECODE_INVALID_PDU           = '\x04'
ATT_ECODE_AUTHENTICATION        = '\x05'
ATT_ECODE_REQ_NOT_SUPP          = '\x06'
ATT_ECODE_INVALID_OFFSET        = '\x07'
ATT_ECODE_AUTHORIZATION         = '\x08'
ATT_ECODE_PREP_QUEUE_FULL       = '\x09'
ATT_ECODE_ATTR_NOT_FOUND        = '\x0A'
ATT_ECODE_ATTR_NOT_LONG         = '\x0B'
ATT_ECODE_INSUFF_ENCR_KEY_SIZE  = '\x0C'
ATT_ECODE_INVAL_ATTR_VALUE_LEN  = '\x0D'
ATT_ECODE_UNLIKELY              = '\x0E'
ATT_ECODE_INSUFF_ENC            = '\x0F'
ATT_ECODE_UNSUPP_GRP_TYPE       = '\x10'
ATT_ECODE_INSUFF_RESOURCES      = '\x11'

GATT_PRIM_SVC_UUID                      = '\x00\x28'
GATT_SND_SVC_UUID                       = '\x01\x28'
GATT_INCLUDE_UUID                       = '\x02\x28'
GATT_CHARAC_UUID                        = '\x03\x28'

GATT_CHARAC_EXT_PROPER_UUID             = '\x00\x29'
GATT_CHARAC_USER_DESC_UUID              = '\x01\x29'
GATT_CLIENT_CHARAC_CFG_UUID             = '\x02\x29'
GATT_SERVER_CHARAC_CFG_UUID             = '\x03\x29'
GATT_CHARAC_FMT_UUID                    = '\x04\x29'
GATT_CHARAC_AGREG_FMT_UUID              = '\x05\x29'
GATT_CHARAC_VALID_RANGE_UUID            = '\x06\x29'
GATT_EXTERNAL_REPORT_REFERENCE          = '\x07\x29'
GATT_REPORT_REFERENCE                   = '\x08\x29'

GATT_CHARAC_DEVICE_NAME                 = '\x00\x2A'
GATT_CHARAC_APPEARANCE                  = '\x01\x2A'
GATT_CHARAC_PERIPHERAL_PRIV_FLAG        = '\x02\x2A'
GATT_CHARAC_RECONNECTION_ADDRESS        = '\x03\x2A'
GATT_CHARAC_PERIPHERAL_PREF_CONN        = '\x04\x2A'
GATT_CHARAC_SERVICE_CHANGED             = '\x05\x2A'
GATT_CHARAC_SYSTEM_ID                   = '\x23\x2A'
GATT_CHARAC_MODEL_NUMBER_STRING         = '\x24\x2A'
GATT_CHARAC_SERIAL_NUMBER_STRING        = '\x25\x2A'
GATT_CHARAC_FIRMWARE_REVISION_STRING    = '\x26\x2A'
GATT_CHARAC_HARDWARE_REVISION_STRING    = '\x27\x2A'
GATT_CHARAC_SOFTWARE_REVISION_STRING    = '\x28\x2A'
GATT_CHARAC_MANUFACTURER_NAME_STRING    = '\x29\x2A'
GATT_CHARAC_PNP_ID                      = '\x50\x2A'


def binUuid(uuid):
    if isinstance(uuid, str):
        check = btle.capitaliseName(uuid)
        if check in btle.AssignedNumbers.__dict__:
            return btle.AssignedNumbers.__dict__[check].bin()
    return btle.UUID(uuid).bin()

def chr2(val):
    if val < 0 or 0xFFFF < val:
        raise Exception
    return struct.pack('<H', val)

class AttError(Exception):
    def __init__(self, err, h = 0):
        self.error = err
        self.handle = h

class Attribute:
    def __init__(self, helper, h, type, value):
        assert(helper == None or isinstance(helper, btle.BluepyHelper))
        assert(isinstance(value, str))

        self.helper = helper
        self.handle = h
        self.type = type
        self._value = value

    # can throw AttError in case read is not supported
    def read(self):
        return self._value

    # Just return None in case of error, but it should not throw exception
    def readSafe(self):
        return self._value

    def write(self, value):
        raise AttError(ATT_ECODE_WRITE_NOT_PERM, self.handle)

    def notify(self, value):
        assert(isinstance(value, str))
        self._value = value
        if self.helper:
            self.helper._writeCmd("gatts %s\n" % binascii.b2a_hex(ATT_OP_HANDLE_NOTIFY + chr2(self.handle) + self._value))
        else:
            print("notify ERROR")

    def indicate(self, value):
        assert(isinstance(value, str))
        self._value = value
        if self.helper:
            self.helper._writeCmd("gatts %s\n" % binascii.b2a_hex(ATT_OP_HANDLE_IND + chr2(self.handle) + self._value))

class Gatts:
    def __init__(self, helper = None, mtu = 23):
        self.helper = helper
        self.mtu = mtu

        # Except index 0, each element must be a valid Attribute (or derivated)
        self.att = [ None ]
        # We build a dict with constants above and functions below
        self.op_func = {}
        for name, value in globals().iteritems():
            if name[0:7] == 'ATT_OP_' and name[-4:] in ['_REQ', '_CMD']:
                self.op_func[value] = getattr(self, name.lower())

    def reconnect(self, addr, addr_type):
        self.prepare_list = {}

    def hmax(self, h = 0xFFFF):
        return min(len(self.att) - 1, h)


    def hend(self, att):
        if att.type not in [GATT_PRIM_SVC_UUID, GATT_SND_SVC_UUID, GATT_CHARAC_UUID]:
            return hstart
        # we stop at the next service, or also at the next char if we start from a char
        type_end = set([GATT_PRIM_SVC_UUID, GATT_SND_SVC_UUID, att.type])
        for a in self.att[att.handle + 1:]:
            if a.type in type_end:
                return a.handle - 1
        return len(self.att) - 1


    def hcheck(self, h1, h2 = None):
        # Different behavior depending on 1 or 2 args
        if h2 is None:
            if h1 <= 0 or len(self.att) <= h1:
                raise AttError(ATT_ECODE_INVALID_HANDLE, h1)
            return self.att[h1]
        else:
            if h1 <= 0 or h2 <= 0 or h2 < h1 or 0xFFFF < h1 or 0xFFFF < h2:
                raise AttError(ATT_ECODE_INVALID_HANDLE, h1)

    def att_op_mtu_req(self,data):
        # Handled at higher level, just here to fill the dict
        return None


    def att_op_find_info_req(self,data):
        op, hstart, hend = struct.unpack("<BHH", data)
        btle.DBG("Find info Req:", op, hstart, hend)
        self.hcheck(hstart, hend)

        rlen = self.mtu - 2
        info_data_list = []
        for att in self.att[hstart : hend + 1]:
            info_data = chr2(att.handle) + att.type
            if info_data_list and len(info_data_list[0]) != len(info_data):
                break
            rlen -= len(info_data)
            if rlen < 0:
                break
            info_data_list += [info_data]

        if not info_data_list:
            raise AttError(ATT_ECODE_ATTR_NOT_FOUND, hstart)
        return ATT_OP_FIND_INFO_RESP + chr(1 if len(info_data_list[0]) == 4 else 2) + ''.join(info_data_list)


    def att_op_find_by_type_req(self,data):
        op, hstart, hend, uuid, value = struct.unpack("<BHH2s" + str(len(data) - 7) + "s", data)
        btle.DBG("Find Type Value Req:", op, hstart, hend, binascii.b2a_hex(uuid), binascii.b2a_hex(value))
        self.hcheck(hstart, hend)

        rlen = self.mtu - 1
        handle_info_list = []
        for att in self.att[hstart : hend + 1]:
            if att.type == uuid and value == att.readSafe():
                handle_info = chr2(att.handle) + chr2(self.hend(att))
                rlen -= len(handle_info)
                if rlen < 0:
                    break
                handle_info_list += [handle_info]

        if not handle_info_list:
            raise AttError(ATT_ECODE_ATTR_NOT_FOUND, hstart)
        return ATT_OP_FIND_BY_TYPE_RESP + ''.join(handle_info_list)


    def att_op_read_req(self,data):
        op, h = struct.unpack("<BH", data)
        btle.DBG("Read Req:", op, h)

        a = self.hcheck(h)
        return ATT_OP_READ_RESP + self.att[h].read()[:self.mtu - 1]


    def att_op_read_blob_req(self,data):
        op, h, off = struct.unpack("<BHH", data)
        btle.DBG("Read Blob Req:", op, h, off)

        a = self.hcheck(h)
        value = a.read()
        if len(value) < off:
            raise AttError(ATT_ECODE_INVALID_OFFSET, h)

        return ATT_OP_READ_BLOB_RESP + value[off:off + self.mtu - 1]


    def att_op_read_multi_req(self,data):
        h = []
        val = ""
        rv = struct.unpack("<B%dH"%((len(data) - 1)/2), data)
        op = rv[0]
        handles = rv[1:]

        btle.DBG("Read Multi Req:", op, str(handles))

        for h in handles:
            a = self.hcheck(h)
            val += self.att[h].read()

        val = val[:self.mtu - 1]

        return ATT_OP_READ_MULTI_RESP + val

    def att_op_read_by_type_req(self,data):
        op, hstart, hend, uuid = struct.unpack("<BHH" + str(len(data) - 5) + "s", data)
        btle.DBG("Read Type Req:", op, hstart, hend, binascii.b2a_hex(uuid))
        self.hcheck(hstart, hend)

        rlen = self.mtu - 2
        att_data_list = []

        for att in self.att[hstart : hend + 1]:
            if uuid == att.type:
                if not att_data_list:
                    val = att.read()
                else:
                    val = att.readSafe()
                    if val is None:
                        continue
                att_data = chr2(att.handle) + val[:min(253, self.mtu - 4)]
                if att_data_list and len(att_data) != len(att_data_list[0]):
                    continue
                rlen -= len(att_data)
                if rlen < 0:
                    break
                att_data_list += [ att_data ]

        if att_data_list:
            return ATT_OP_READ_BY_TYPE_RESP + chr(len(att_data_list[0])) + ''.join(att_data_list)
        else:
            raise AttError(ATT_ECODE_ATTR_NOT_FOUND, hstart)


    def att_op_read_by_group_req(self,data):
        op, hstart, hend, uuid = struct.unpack("<BHH" + str(len(data) - 5) + "s", data)
        btle.DBG("Read Group Type Req:", op, hstart, hend, binascii.b2a_hex(uuid))
        self.hcheck(hstart, hend)

        if uuid not in [GATT_PRIM_SVC_UUID, GATT_SND_SVC_UUID, GATT_CHARAC_UUID]:
            raise AttError(ATT_ECODE_UNSUPP_GRP_TYPE, hstart)

        rlen = self.mtu - 2
        att_data_list = []

        for att in self.att[hstart : hend + 1]:
            if uuid == att.type:
                if not att_data_list:
                    val = att.read()
                else:
                    val = att.readSafe()
                    if val is None:
                        continue
                att_data = chr2(att.handle) + chr2(self.hend(att)) + val[:min(251, self.mtu - 6)]
                if att_data_list and len(att_data) != len(att_data_list[0]):
                    continue
                rlen -= len(att_data)
                if rlen < 0:
                    break
                att_data_list += [ att_data ]

        if att_data_list:
            return ATT_OP_READ_BY_GROUP_RESP + chr(len(att_data_list[0])) + ''.join(att_data_list)
        else:
            raise AttError(ATT_ECODE_ATTR_NOT_FOUND, hstart)


    def att_op_write_req(self,data):
        op, h = struct.unpack("<BH", data[:3])
        d = data[3:]

        a = self.hcheck(h)
        a.write(d)
        return ATT_OP_WRITE_RESP


    def att_op_write_cmd(self,data):
        op, h = struct.unpack("<BH", data[:3])
        d = data[3:]

        a = self.hcheck(h)
        a.write(d)
        return None


    def att_op_prep_write_req(self,data):
        op, h, off = struct.unpack("<BHH", data[:5])
        a = self.hcheck(h)
        d = {"off" : off, "data" : data[5:]}
        # Append d to key h, or create it if not existing
        self.prepare_list.setdefault(h,[]).append(d)
        return ATT_OP_PREP_WRITE_RESP + data[1:]


    def att_op_exec_write_req(self,data):
        op, flags = struct.unpack("<BB", data[:2])
        for h in self.prepare_list.keys():
            a = self.hcheck(h)
            for d in self.prepare_list[h]:
                try:
                    if len(data) < d["off"]:
                        data += "\0" * (d["off"] - len(data))
                except NameError:
                    data = "\0" * d["off"]
                data = data[:d["off"]] + d["data"] + data[d["off"] + len(d["data"]):]
            a.write(data)
        return ATT_OP_EXEC_WRITE_RESP


    def att_op_signed_write_cmd(self,data):
        btle.DBG("Req SIGNED WRITE not supported: " + binascii.b2a_hex(data))
        raise AttError(ATT_ECODE_REQ_NOT_SUPP, 0)


    def att_op_unknown(self,data):
        btle.DBG("Req unknown: " + binascii.b2a_hex(data))
        raise AttError(ATT_ECODE_REQ_NOT_SUPP, 0)


    def __call__(self, data):
        try:
            op_func = self.op_func.get(data[0], self.att_op_unknown)
            return op_func(data)
        except AttError as e:
            return ATT_OP_ERROR + data[0] + chr2(e.handle) + e.error


    def addService(self, uuid):
        return self.addDesc("GATT Primary Service Declaration", binUuid(uuid))

    def addInclService(self, sh, eh, uuid):
        return self.addDesc("GATT Include Declaration", chr2(sh) + chr2(eh) + binUuid(uuid))

    def addChar(self, uuid, value, prop = ["READ"]):
        if isinstance(prop, int):
            binprop = chr(prop)
            strprop = [ btle.Characteristic.props[1<<pos] for pos in xrange(0,8) if (prop & (1<<pos))]
        else:
            if isinstance(prop, str):
                prop = prop.split()
            strprop = prop
            intprop = 0
            for p in prop:
                intprop |= btle.Characteristic.props[p]
            binprop = chr(intprop)

        # handle of the characteristic value descriptor (next)
        binhandle = chr2(len(self.att) + 1)
        binuuid = binUuid(uuid)

        self.addDesc("GATT Characteristic Declaration", binprop + binhandle + binuuid)
        self.addDesc(uuid, value)
        return (self.att[-2], self.att[-1])


    def addDesc(self, uuid, value):
        self.att += [ Attribute(self.helper, len(self.att), binUuid(uuid), value) ]
        return self.att[-1]
