import time
from threading import Thread
from bluepy.sensortag import SensorTag


class SensorTagCollector(Thread):
    """
    SensorTag Collector Thread to always collect sensor metrics even in-case of dis-connectivity
    due to external factors, without any need of manual intervention.

    This simply prints values in console.  In automated agents, a queue can be used
    for every sensor and retrieve values from it.
    """

    def __init__(self, device_name, device_mac, sampling_interval_sec=1, retry_interval_sec=5):
        """
        :param device_name: SensorTag Device's name
        :param device_mac: SensorTag's MAC Address
        :param sampling_interval_sec: Time interval in seconds to collect metrics from sensors
        :param retry_interval_sec: Time interval in seconds to wait before attempting to retry establishing connection.
        """
        Thread.__init__(self)
        self.daemon = True
        self.tag = None
        self.device_name = device_name
        self.device_mac = device_mac
        self._sampling_interval_sec = sampling_interval_sec
        self._retry_interval_sec = retry_interval_sec
        # Connects with re-try mechanism
        self._re_connect()
        self.start()

    def _connect(self):
        """
        Connects with a SensorTag Device.
        :return: None
        """
        print "Connecting..."
        self.tag = SensorTag(self.device_mac)
        print "Connected..."
        self._enable()

    def _enable(self):
        """
        Enables all Sensors.
        :return: None
        """
        # Enabling selected sensors
        self.tag.IRtemperature.enable()
        self.tag.humidity.enable()
        self.tag.accelerometer.enable()
        self.tag.magnetometer.enable()
        self.tag.gyroscope.enable()
        self.tag.battery.enable()
        self.tag.keypress.enable()
        if self.tag.lightmeter is None:
            print("Warning: no lightmeter on this device")
        else:
            self.tag.lightmeter.enable()

        # Some sensors (e.g., temperature, accelerometer) need some time for initialization.
        # Not waiting here after enabling a sensor, the first read value might be empty or incorrect.
        time.sleep(1)
        print "Enabled all Sensors.."

    def run(self):
        """
        Prints values read from each sensor from a single SensorTag Device.

        In case of a dis-connectivity, it tries to re-connect automatically.
        :return: None
        """
        while True:
            while True:
                try:
                    print(self.device_name + " Temp: ", self.tag.IRtemperature.read())
                    print(self.device_name + " Humidity: ", self.tag.humidity.read())
                    print(self.device_name + " Barometer: ", self.tag.barometer.read())
                    print(self.device_name + " Accelerometer: ", self.tag.accelerometer.read())
                    print(self.device_name + " Magnetometer: ", self.tag.magnetometer.read())
                    print(self.device_name + " Gyroscope: ", self.tag.gyroscope.read())
                    print(self.device_name + " Battery: ", self.tag.battery.read())
                    if self.tag.lightmeter is not None:
                        print(self.device_name + " Light: ", self.tag.lightmeter.read())

                    self.tag.waitForNotifications(self._sampling_interval_sec)
                except Exception as e:
                    print str(e)
                    self.tag.disconnect()
                    break

            time.sleep(self._retry_interval_sec)
            self._re_connect()

    def _re_connect(self):
        """
        Reconnects with a SensorTag Device
        :return:
        """
        while True:
            try:
                self._connect()
                break
            except Exception as e:
                print str(e)
                time.sleep(self._retry_interval_sec)


if __name__ == '__main__':

    SensorTagCollector(device_name="MySensorTag", device_mac="DEVICE_MAC", sampling_interval_sec=1)

    while True:
        pass
