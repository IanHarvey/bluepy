from bluepy.btle import Scanner, DefaultDelegate, Peripheral
import struct
import sys
import time
import sys
import threading

class sensorDelegate(DefaultDelegate):
	def __init__(self, deviceId):
		DefaultDelegate.__init__(self)
		self.deviceId = deviceId
	def handleNotification(self, cHandle, data):
		# process your data here
		print data
		pass

class bleHost(threading.Thread):
    def __init__(self, deviceId, name, addr, addrType):
        threading.Thread.__init__(self)
        self.deviceId = deviceId
        self.name = name
        self.addr = addr
        self.addrType = addrType

        # replace the uuid here
        self.srv_uuid  = '6e400001-b5a3-f393-e0a9-e50e24dcca9e' # server uuid
        self.sDelegate = sensorDelegate(self.deviceId)

    def run(self):
        while True:
            try:
                print '[Bluetooth]\tConnecting to BLE device %s(%s)...'%(self.addr, self.addrType)
                per = Peripheral(self.addr, self.addrType)
                print '[Bluetooth]\tBLE device %s(%s) Connected.'%(self.addr, self.addrType)

                per.withDelegate(self.sDelegate)
            except KeyboardInterrupt:
                print '[Bluetooth]\tKeyboard interrupt!'
                break
            except Exception, e:
                print '[Bluetooth]\terror:', e
                time.sleep(1)
                continue
            try:
                srv = per.getServiceByUUID(self.srv_uuid)

                chrc = srv.getCharacteristics(self.acc_uuid)[0]
				# send 1 to allow notification
                per.writeCharacteristic(chrc.getHandle() + 1, struct.pack('<bb', 0x01, 0x00), True)
            except Exception, e:
                print '[Bluetooth]\terror:', e
                time.sleep(1)
                continue

        while True:
            try:
                if per.waitForNotifications(1.0):
					pass
            except KeyboardInterrupt:
                print '[Bluetooth]\tKeyboard interrupt!'
                break
            except Exception, e:
                print '[Bluetooth]\terror:', e
                break
        try:
            per.disconnect()
        except Exception, e:
            print '[Bluetooth]\terror:', e

if __name__ == '__main__':
	argc = len(sys.argv) 
	if argc < 2:
		addr1 = 'fc:8b:2e:c0:4e:fa'
		addr1Type = 'random'
	elif argc == 2:
		addr1 = sys.argv[1]
		addr1Type = 'random'
	elif argc == 3:
		addr1 = sys.argv[1]
		addr1Type = sys.argv[2]
	
	bleThread1 = bleHost('sensor1', 'BleThread1', addr1, addr1Type)
	bleThread1.start()
