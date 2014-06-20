from btle import UUID, Peripheral
import struct
import math

def _TI_UUID(val):
    return UUID("%08X-0451-4000-b000-000000000000" % (0xF0000000+val))

class SensorBase:
    # Derived classes should set: svcUUID, ctrlUUID, dataUUID
    sensorOn  = chr(0x01)
    sensorOff = chr(0x00)

    def __init__(self, periph):
        self.periph = periph
        self.service = self.periph.getServiceByUUID(self.svcUUID)
        self.ctrl = None
        self.data = None

    def enable(self):
        if self.ctrl is None:
            self.ctrl = self.service.getCharacteristics(self.ctrlUUID) [0]
        if self.data is None:
            self.data = self.service.getCharacteristics(self.dataUUID) [0]
        if self.sensorOn is not None:
            self.ctrl.write(self.sensorOn,withResponse=True)

    def read(self):
        return self.data.read()

    def disable(self):
        if self.ctrl is not None:
            self.ctrl.write(self.sensorOff)

    # Derived class should implement _formatData()

def calcPoly(coeffs, x):
    return coeffs[0] + (coeffs[1]*x) + (coeffs[2]*x*x)

class IRTemperatureSensor(SensorBase):
    svcUUID  = _TI_UUID(0xAA00)
    dataUUID = _TI_UUID(0xAA01)
    ctrlUUID = _TI_UUID(0xAA02)

    zeroC = 273.15 # Kelvin
    tRef  = 298.15
    Apoly = [1.0,      1.75e-3, -1.678e-5]
    Bpoly = [-2.94e-5, -5.7e-7,  4.63e-9]
    Cpoly = [0.0,      1.0,      13.4]

    def __init__(self, periph):
        SensorBase.__init__(self, periph)
        self.S0 = 6.4e-14

    def read(self):
        '''Returns (ambient_temp, target_temp) in degC'''

        # See http://processors.wiki.ti.com/index.php/SensorTag_User_Guide#IR_Temperature_Sensor
        (rawVobj, rawTamb) = struct.unpack('<hh', self.data.read())
        tAmb = rawTamb / 128.0
        Vobj = 1.5625e-7 * rawVobj

        tDie = tAmb + self.zeroC
        S   = self.S0 * calcPoly(self.Apoly, tDie-self.tRef)
        Vos = calcPoly(self.Bpoly, tDie-self.tRef)
        fObj = calcPoly(self.Cpoly, Vobj-Vos)

        tObj = math.pow( math.pow(tDie,4.0) + (fObj/S), 0.25 )
        return (tAmb, tObj - self.zeroC)


class AccelerometerSensor(SensorBase):
    svcUUID  = _TI_UUID(0xAA10)
    dataUUID = _TI_UUID(0xAA11)
    ctrlUUID = _TI_UUID(0xAA12)

    def __init__(self, periph):
        SensorBase.__init__(self, periph)

    def read(self):
        '''Returns (x_accel, y_accel, z_accel) in units of g'''
        x_y_z = struct.unpack('bbb', self.data.read())
        return tuple([ (val/64.0) for val in x_y_z ])

class HumiditySensor(SensorBase):
    svcUUID  = _TI_UUID(0xAA20)
    dataUUID = _TI_UUID(0xAA21)
    ctrlUUID = _TI_UUID(0xAA22)

    def __init__(self, periph):
        SensorBase.__init__(self, periph)

    def read(self):
        '''Returns (ambient_temp, rel_humidity)'''
        (rawT, rawH) = struct.unpack('<HH', self.data.read())
        temp = -46.85 + 175.72 * (rawT / 65536.0)
        RH = -6.0 + 125.0 * ((rawH & 0xFFFC)/65536.0)
        return (temp, RH)


class MagnetometerSensor(SensorBase):
    svcUUID  = _TI_UUID(0xAA30)
    dataUUID = _TI_UUID(0xAA31)
    ctrlUUID = _TI_UUID(0xAA32)

    def __init__(self, periph):
        SensorBase.__init__(self, periph)

    def read(self):
        '''Returns (x, y, z) in uT units'''
        x_y_z = struct.unpack('<hhh', self.data.read())
        return tuple([ 1000.0 * (v/32768.0) for v in x_y_z ])
        # Revisit - some absolute calibration is needed

class BarometerSensor(SensorBase):
    svcUUID  = _TI_UUID(0xAA40)
    dataUUID = _TI_UUID(0xAA41)
    ctrlUUID = _TI_UUID(0xAA42)
    calUUID  = _TI_UUID(0xAA43)
    sensorOn = None

    def __init__(self, periph):
       SensorBase.__init__(self, periph)

    def enable(self):
        SensorBase.enable(self)
        self.calChr = self.service.getCharacteristics(self.calUUID) [0]

        # Read calibration data
        self.ctrl.write( chr(0x02), True )
        (c1,c2,c3,c4,c5,c6,c7,c8) = struct.unpack("<HHHHhhhh", self.calChr.read())
        self.c1_s = c1/float(1 << 24)
        self.c2_s = c2/float(1 << 10)
        self.sensPoly = [ c3/1.0, c4/float(1 << 17), c5/float(1<<34) ]
        self.offsPoly = [ c6*float(1<<14), c7/8.0, c8/float(1<<19) ]
        self.ctrl.write( chr(0x01), True )


    def read(self):
        '''Returns (ambient_temp, pressure_millibars)'''
        (rawT, rawP) = struct.unpack('<hH', self.data.read())
        temp = (self.c1_s * rawT) + self.c2_s
        sens = calcPoly( self.sensPoly, float(rawT) )
        offs = calcPoly( self.offsPoly, float(rawT) )
        pres = (sens * rawP + offs) / (100.0 * float(1<<14))
        return (temp,pres)


class GyroscopeSensor(SensorBase):
    svcUUID  = _TI_UUID(0xAA50)
    dataUUID = _TI_UUID(0xAA51)
    ctrlUUID = _TI_UUID(0xAA52)
    sensorOn = chr(0x07)

    def __init__(self, periph):
       SensorBase.__init__(self, periph)

    def read(self):
        '''Returns (x,y,z) rate in deg/sec'''
        x_y_z = struct.unpack('<hhh', self.data.read())
        return tuple([ 250.0 * (v/32768.0) for v in x_y_z ])

#class KeypressSensor(SensorBase):
# TODO: only sends notifications, you can't poll it
#    svcUUID = UUID(0xFFE0)
# write 0100 to 0x60
# get notifications on 5F

class SensorTag(Peripheral):
    def __init__(self,addr):
        Peripheral.__init__(self,addr)
        self.discoverServices()
        self.IRtemperature = IRTemperatureSensor(self)
        self.accelerometer = AccelerometerSensor(self)
        self.humidity = HumiditySensor(self)
        self.magnetometer = MagnetometerSensor(self)
        self.barometer = BarometerSensor(self)
        self.gyroscope = GyroscopeSensor(self)
        # self.keypress = KeypressSensor(self)

if __name__ == "__main__":
    import time

    def quickTest(sensor):
        sensor.enable()
        for i in range(10):
            print("Result", sensor.read())
            time.sleep(1.0)
        sensor.disable()

    tag = SensorTag("BC:6A:29:AB:D3:7A")

    sensors = [tag.IRtemperature, tag.humidity, tag.barometer]
    [ s.enable() for s in sensors ]

    while True:
       ir, hum, baro = [ s.read() for s in sensors ]
       print("IR", ir, "hum", hum, "baro", baro)
       time.sleep(5.0)

    tag.disconnect()
    del tag
