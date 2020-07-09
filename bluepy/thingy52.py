from bluepy.btle import UUID, Peripheral, ADDR_TYPE_RANDOM, DefaultDelegate
import argparse
import time
import binascii

def write_uint16(data, value, index):
    """ Write 16bit value into data string at index and return new string """
    data = data.decode('utf-8')  # This line is added to make sure both Python 2 and 3 works
    return '{}{:02x}{:02x}{}'.format(
                data[:index*4], 
                value & 0xFF, value >> 8, 
                data[index*4 + 4:])

def write_uint8(data, value, index):
    """ Write 8bit value into data string at index and return new string """
    data = data.decode('utf-8')  # This line is added to make sure both Python 2 and 3 works
    return '{}{:02x}{}'.format(
                data[:index*2], 
                value, 
                data[index*2 + 2:])

# Please see # Ref https://nordicsemiconductor.github.io/Nordic-Thingy52-FW/documentation
# for more information on the UUIDs of the Services and Characteristics that are being used
def Nordic_UUID(val):
    """ Adds base UUID and inserts value to return Nordic UUID """
    return UUID("EF68%04X-9B35-4933-9B10-52FFA9740042" % val)

# Definition of all UUID used by Thingy
CCCD_UUID = 0x2902

BATTERY_SERVICE_UUID = 0x180F
BATTERY_LEVEL_UUID = 0x2A19

ENVIRONMENT_SERVICE_UUID = 0x0200
E_TEMPERATURE_CHAR_UUID = 0x0201
E_PRESSURE_CHAR_UUID    = 0x0202
E_HUMIDITY_CHAR_UUID    = 0x0203
E_GAS_CHAR_UUID         = 0x0204
E_COLOR_CHAR_UUID       = 0x0205
E_CONFIG_CHAR_UUID      = 0x0206

USER_INTERFACE_SERVICE_UUID = 0x0300
UI_LED_CHAR_UUID            = 0x0301
UI_BUTTON_CHAR_UUID         = 0x0302
UI_EXT_PIN_CHAR_UUID        = 0x0303

MOTION_SERVICE_UUID         = 0x0400
M_CONFIG_CHAR_UUID          = 0x0401
M_TAP_CHAR_UUID             = 0x0402
M_ORIENTATION_CHAR_UUID     = 0x0403
M_QUATERNION_CHAR_UUID      = 0x0404
M_STEP_COUNTER_UUID         = 0x0405
M_RAW_DATA_CHAR_UUID        = 0x0406
M_EULER_CHAR_UUID           = 0x0407
M_ROTATION_MATRIX_CHAR_UUID = 0x0408
M_HEAIDNG_CHAR_UUID         = 0x0409
M_GRAVITY_VECTOR_CHAR_UUID  = 0x040A

SOUND_SERVICE_UUID          = 0x0500
S_CONFIG_CHAR_UUID          = 0x0501
S_SPEAKER_DATA_CHAR_UUID    = 0x0502
S_SPEAKER_STATUS_CHAR_UUID  = 0x0503
S_MICROPHONE_CHAR_UUID      = 0x0504

# Notification handles used in notification delegate
e_temperature_handle = None
e_pressure_handle = None
e_humidity_handle = None
e_gas_handle = None
e_color_handle = None
ui_button_handle = None
m_tap_handle = None
m_orient_handle = None
m_quaternion_handle = None
m_stepcnt_handle = None
m_rawdata_handle = None
m_euler_handle = None
m_rotation_handle = None
m_heading_handle = None
m_gravity_handle = None
s_speaker_status_handle = None
s_microphone_handle = None


class BatterySensor():
    """
    Battery Service module. Instance the class and enable to get access to Battery interface.
    """
    svcUUID = UUID(BATTERY_SERVICE_UUID)  # Ref https://www.bluetooth.com/specifications/gatt/services 
    dataUUID = UUID(BATTERY_LEVEL_UUID) # Ref https://www.bluetooth.com/specifications/gatt/characteristics

    def __init__(self, periph):
        self.periph = periph
        self.service = None
        self.data = None

    def enable(self):
        """ Enables the class by finding the service and its characteristics. """
        if self.service is None:
            self.service = self.periph.getServiceByUUID(self.svcUUID)
        if self.data is None:
            self.data = self.service.getCharacteristics(self.dataUUID)[0]

    def read(self):
        """ Returns the battery level in percent """
        val = ord(self.data.read())
        return val


class EnvironmentService():
    """
    Environment service module. Instance the class and enable to get access to the Environment interface.
    """
    serviceUUID =           Nordic_UUID(ENVIRONMENT_SERVICE_UUID)
    temperature_char_uuid = Nordic_UUID(E_TEMPERATURE_CHAR_UUID)
    pressure_char_uuid =    Nordic_UUID(E_PRESSURE_CHAR_UUID)
    humidity_char_uuid =    Nordic_UUID(E_HUMIDITY_CHAR_UUID)
    gas_char_uuid =         Nordic_UUID(E_GAS_CHAR_UUID)
    color_char_uuid =       Nordic_UUID(E_COLOR_CHAR_UUID)
    config_char_uuid =      Nordic_UUID(E_CONFIG_CHAR_UUID)

    def __init__(self, periph):
        self.periph = periph
        self.environment_service = None
        self.temperature_char = None
        self.temperature_cccd = None
        self.pressure_char = None
        self.pressure_cccd = None
        self.humidity_char = None
        self.humidity_cccd = None
        self.gas_char = None
        self.gas_cccd = None
        self.color_char = None
        self.color_cccd = None
        self.config_char = None

    def enable(self):
        """ Enables the class by finding the service and its characteristics. """
        global e_temperature_handle
        global e_pressure_handle
        global e_humidity_handle
        global e_gas_handle
        global e_color_handle

        if self.environment_service is None:
            self.environment_service = self.periph.getServiceByUUID(self.serviceUUID)
        if self.temperature_char is None:
            self.temperature_char = self.environment_service.getCharacteristics(self.temperature_char_uuid)[0]
            e_temperature_handle = self.temperature_char.getHandle()
            self.temperature_cccd = self.temperature_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.pressure_char is None:
            self.pressure_char = self.environment_service.getCharacteristics(self.pressure_char_uuid)[0]
            e_pressure_handle = self.pressure_char.getHandle()
            self.pressure_cccd = self.pressure_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.humidity_char is None:
            self.humidity_char = self.environment_service.getCharacteristics(self.humidity_char_uuid)[0]
            e_humidity_handle = self.humidity_char.getHandle()
            self.humidity_cccd = self.humidity_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.gas_char is None:
            self.gas_char = self.environment_service.getCharacteristics(self.gas_char_uuid)[0]
            e_gas_handle = self.gas_char.getHandle()
            self.gas_cccd = self.gas_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.color_char is None:
            self.color_char = self.environment_service.getCharacteristics(self.color_char_uuid)[0]
            e_color_handle = self.color_char.getHandle()
            self.color_cccd = self.color_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.config_char is None:
            self.config_char = self.environment_service.getCharacteristics(self.config_char_uuid)[0]

    def set_temperature_notification(self, state):
        if self.temperature_cccd is not None:
            if state == True:
                self.temperature_cccd.write(b"\x01\x00", True)
            else:
                self.temperature_cccd.write(b"\x00\x00", True)

    def set_pressure_notification(self, state):
        if self.pressure_cccd is not None:
            if state == True:
                self.pressure_cccd.write(b"\x01\x00", True)
            else:
                self.pressure_cccd.write(b"\x00\x00", True)

    def set_humidity_notification(self, state):
        if self.humidity_cccd is not None:
            if state == True:
                self.humidity_cccd.write(b"\x01\x00", True)
            else:
                self.humidity_cccd.write(b"\x00\x00", True)

    def set_gas_notification(self, state):
        if self.gas_cccd is not None:
            if state == True:
                self.gas_cccd.write(b"\x01\x00", True)
            else:
                self.gas_cccd.write(b"\x00\x00", True)

    def set_color_notification(self, state):
        if self.color_cccd is not None:
            if state == True:
                self.color_cccd.write(b"\x01\x00", True)
            else:
                self.color_cccd.write(b"\x00\x00", True)

    def configure(self, temp_int=None, press_int=None, humid_int=None, gas_mode_int=None,
                        color_int=None, color_sens_calib=None):
        if temp_int is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint16(current_config, temp_int, 0)
            self.config_char.write(binascii.a2b_hex(new_config), True)
        if press_int is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint16(current_config, press_int, 1)
            self.config_char.write(binascii.a2b_hex(new_config), True)
        if humid_int is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint16(current_config, humid_int, 2)
            self.config_char.write(binascii.a2b_hex(new_config), True)
        if gas_mode_int is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint8(current_config, gas_mode_int, 8)
            self.config_char.write(binascii.a2b_hex(new_config), True)
        if color_int is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint16(current_config, color_int, 3)
            self.config_char.write(binascii.a2b_hex(new_config), True)
        if color_sens_calib is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint8(current_config, color_sens_calib[0], 9)
            new_config = write_uint8(current_config, color_sens_calib[1], 10)
            new_config = write_uint8(current_config, color_sens_calib[2], 11)
            self.config_char.write(binascii.a2b_hex(new_config), True)

    def disable(self):
        self.set_temperature_notification(False)
        self.set_pressure_notification(False)
        self.set_humidity_notification(False)
        self.set_gas_notification(False)
        self.set_color_notification(False)


class UserInterfaceService():
    """
    User interface service module. Instance the class and enable to get access to the UI interface.
    """
    serviceUUID = Nordic_UUID(USER_INTERFACE_SERVICE_UUID)
    led_char_uuid = Nordic_UUID(UI_LED_CHAR_UUID)
    btn_char_uuid = Nordic_UUID(UI_BUTTON_CHAR_UUID)
    # To be added: EXT PIN CHAR

    def __init__(self, periph):
        self.periph = periph
        self.ui_service = None
        self.led_char = None
        self.btn_char = None
        self.btn_char_cccd = None
        # To be added: EXT PIN CHAR

    def enable(self):
        """ Enables the class by finding the service and its characteristics. """
        global ui_button_handle

        if self.ui_service is None:
            self.ui_service = self.periph.getServiceByUUID(self.serviceUUID)
        if self.led_char is None:
            self.led_char = self.ui_service.getCharacteristics(self.led_char_uuid)[0]
        if self.btn_char is None:
            self.btn_char = self.ui_service.getCharacteristics(self.btn_char_uuid)[0]
            ui_button_handle = self.btn_char.getHandle()
            self.btn_char_cccd = self.btn_char.getDescriptors(forUUID=CCCD_UUID)[0]

    def set_led_mode_off(self):
        self.led_char.write(b"\x00", True)
        
    def set_led_mode_constant(self, r, g, b):
        teptep = "01{:02X}{:02X}{:02X}".format(r, g, b)
        self.led_char.write(binascii.a2b_hex(teptep), True)
        
    def set_led_mode_breathe(self, color, intensity, delay):
        """
        Set LED to breathe mode.
        color has to be within 0x01 and 0x07
        intensity [%] has to be within 1-100
        delay [ms] has to be within 1 ms - 10 s
        """
        teptep = "02{:02X}{:02X}{:02X}{:02X}".format(color, intensity,
                delay & 0xFF, delay >> 8)
        self.led_char.write(binascii.a2b_hex(teptep), True)
        
    def set_led_mode_one_shot(self, color, intensity):  
        """
        Set LED to one shot mode.
        color has to be within 0x01 and 0x07
        intensity [%] has to be within 1-100
        """
        teptep = "03{:02X}{:02X}".format(color, intensity)
        self.led_char.write(binascii.a2b_hex(teptep), True)

    def set_btn_notification(self, state):
        if self.btn_char_cccd is not None:
            if state == True:
                self.btn_char_cccd.write(b"\x01\x00", True)
            else:
                self.btn_char_cccd.write(b"\x00\x00", True)

    def disable(self):
        self.set_btn_notification(False)


class MotionService():
    """
    Motion service module. Instance the class and enable to get access to the Motion interface.
    """
    serviceUUID =           Nordic_UUID(MOTION_SERVICE_UUID)
    config_char_uuid =      Nordic_UUID(M_CONFIG_CHAR_UUID)
    tap_char_uuid =         Nordic_UUID(M_TAP_CHAR_UUID)
    orient_char_uuid =      Nordic_UUID(M_ORIENTATION_CHAR_UUID)
    quaternion_char_uuid =  Nordic_UUID(M_QUATERNION_CHAR_UUID)
    stepcnt_char_uuid =     Nordic_UUID(M_STEP_COUNTER_UUID)
    rawdata_char_uuid =     Nordic_UUID(M_RAW_DATA_CHAR_UUID)
    euler_char_uuid =       Nordic_UUID(M_EULER_CHAR_UUID)
    rotation_char_uuid =    Nordic_UUID(M_ROTATION_MATRIX_CHAR_UUID)
    heading_char_uuid =     Nordic_UUID(M_HEAIDNG_CHAR_UUID)
    gravity_char_uuid =     Nordic_UUID(M_GRAVITY_VECTOR_CHAR_UUID)

    def __init__(self, periph):
        self.periph = periph
        self.motion_service = None
        self.config_char = None
        self.tap_char = None
        self.tap_char_cccd = None
        self.orient_char = None
        self.orient_cccd = None
        self.quaternion_char = None
        self.quaternion_cccd = None
        self.stepcnt_char = None
        self.stepcnt_cccd = None
        self.rawdata_char = None
        self.rawdata_cccd = None
        self.euler_char = None
        self.euler_cccd = None
        self.rotation_char = None
        self.rotation_cccd = None
        self.heading_char = None
        self.heading_cccd = None
        self.gravity_char = None
        self.gravity_cccd = None

    def enable(self):
        """ Enables the class by finding the service and its characteristics. """
        global m_tap_handle
        global m_orient_handle
        global m_quaternion_handle
        global m_stepcnt_handle
        global m_rawdata_handle
        global m_euler_handle
        global m_rotation_handle
        global m_heading_handle
        global m_gravity_handle

        if self.motion_service is None:
            self.motion_service = self.periph.getServiceByUUID(self.serviceUUID)
        if self.config_char is None:
            self.config_char = self.motion_service.getCharacteristics(self.config_char_uuid)[0]
        if self.tap_char is None:
            self.tap_char = self.motion_service.getCharacteristics(self.tap_char_uuid)[0]
            m_tap_handle = self.tap_char.getHandle()
            self.tap_char_cccd = self.tap_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.orient_char is None:
            self.orient_char = self.motion_service.getCharacteristics(self.orient_char_uuid)[0]
            m_orient_handle = self.orient_char.getHandle()
            self.orient_cccd = self.orient_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.quaternion_char is None:
            self.quaternion_char = self.motion_service.getCharacteristics(self.quaternion_char_uuid)[0]
            m_quaternion_handle = self.quaternion_char.getHandle()
            self.quaternion_cccd = self.quaternion_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.stepcnt_char is None:
            self.stepcnt_char = self.motion_service.getCharacteristics(self.stepcnt_char_uuid)[0]
            m_stepcnt_handle = self.stepcnt_char.getHandle()
            self.stepcnt_cccd = self.stepcnt_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.rawdata_char is None:
            self.rawdata_char = self.motion_service.getCharacteristics(self.rawdata_char_uuid)[0]
            m_rawdata_handle = self.rawdata_char.getHandle()
            self.rawdata_cccd = self.rawdata_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.euler_char is None:
            self.euler_char = self.motion_service.getCharacteristics(self.euler_char_uuid)[0]
            m_euler_handle = self.euler_char.getHandle()
            self.euler_cccd = self.euler_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.rotation_char is None:
            self.rotation_char = self.motion_service.getCharacteristics(self.rotation_char_uuid)[0]
            m_rotation_handle = self.rotation_char.getHandle()
            self.rotation_cccd = self.rotation_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.heading_char is None:
            self.heading_char = self.motion_service.getCharacteristics(self.heading_char_uuid)[0]
            m_heading_handle = self.heading_char.getHandle()
            self.heading_cccd = self.heading_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.gravity_char is None:
            self.gravity_char = self.motion_service.getCharacteristics(self.gravity_char_uuid)[0]
            m_gravity_handle = self.gravity_char.getHandle()
            self.gravity_cccd = self.gravity_char.getDescriptors(forUUID=CCCD_UUID)[0]

    def set_tap_notification(self, state):
        if self.tap_char_cccd is not None:
            if state == True:
                self.tap_char_cccd.write(b"\x01\x00", True)
            else:
                self.tap_char_cccd.write(b"\x00\x00", True)

    def set_orient_notification(self, state):
        if self.orient_cccd is not None:
            if state == True:
                self.orient_cccd.write(b"\x01\x00", True)
            else:
                self.orient_cccd.write(b"\x00\x00", True)

    def set_quaternion_notification(self, state):
        if self.quaternion_cccd is not None:
            if state == True:
                self.quaternion_cccd.write(b"\x01\x00", True)
            else:
                self.quaternion_cccd.write(b"\x00\x00", True)

    def set_stepcnt_notification(self, state):
        if self.stepcnt_cccd is not None:
            if state == True:
                self.stepcnt_cccd.write(b"\x01\x00", True)
            else:
                self.stepcnt_cccd.write(b"\x00\x00", True)

    def set_rawdata_notification(self, state):
        if self.rawdata_cccd is not None:
            if state == True:
                self.rawdata_cccd.write(b"\x01\x00", True)
            else:
                self.rawdata_cccd.write(b"\x00\x00", True)

    def set_euler_notification(self, state):
        if self.euler_cccd is not None:
            if state == True:
                self.euler_cccd.write(b"\x01\x00", True)
            else:
                self.euler_cccd.write(b"\x00\x00", True)

    def set_rotation_notification(self, state):
        if self.rotation_cccd is not None:
            if state == True:
                self.rotation_cccd.write(b"\x01\x00", True)
            else:
                self.rotation_cccd.write(b"\x00\x00", True)

    def set_heading_notification(self, state):
        if self.heading_cccd is not None:
            if state == True:
                self.heading_cccd.write(b"\x01\x00", True)
            else:
                self.heading_cccd.write(b"\x00\x00", True)

    def set_gravity_notification(self, state):
        if self.gravity_cccd is not None:
            if state == True:
                self.gravity_cccd.write(b"\x01\x00", True)
            else:
                self.gravity_cccd.write(b"\x00\x00", True)

    def configure(self, step_int=None, temp_comp_int=None, magnet_comp_int=None,
                        motion_freq=None, wake_on_motion=None):
        if step_int is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint16(current_config, step_int, 0)
            self.config_char.write(binascii.a2b_hex(new_config), True)
        if temp_comp_int is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint16(current_config, temp_comp_int, 1)
            self.config_char.write(binascii.a2b_hex(new_config), True)
        if magnet_comp_int is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint16(current_config, magnet_comp_int, 2)
            self.config_char.write(binascii.a2b_hex(new_config), True)
        if motion_freq is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint16(current_config, motion_freq, 3)
            self.config_char.write(binascii.a2b_hex(new_config), True)
        if wake_on_motion is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint8(current_config, wake_on_motion, 8)
            self.config_char.write(binascii.a2b_hex(new_config), True)

    def disable(self):
        self.set_tap_notification(False)
        self.set_orient_notification(False)
        self.set_quaternion_notification(False)
        self.set_stepcnt_notification(False)
        self.set_rawdata_notification(False)
        self.set_euler_notification(False)
        self.set_rotation_notification(False)
        self.set_heading_notification(False)
        self.set_gravity_notification(False)


class SoundService():
    """
    Sound service module. Instance the class and enable to get access to the Sound interface.
    """
    serviceUUID                 = Nordic_UUID(SOUND_SERVICE_UUID)
    config_char_uuid            = Nordic_UUID(S_CONFIG_CHAR_UUID)
    speaker_data_char_uuid      = Nordic_UUID(S_SPEAKER_DATA_CHAR_UUID)
    speaker_status_char_uuid    = Nordic_UUID(S_SPEAKER_STATUS_CHAR_UUID)
    microphone_char_uuid        = Nordic_UUID(S_MICROPHONE_CHAR_UUID)

    def __init__(self, periph):
        self.periph = periph
        self.sound_service = None
        self.config_char = None
        self.speaker_data_char = None
        self.speaker_status_char = None
        self.speaker_status_char_cccd = None
        self.microphone_char = None
        self.microphone_char_cccd = None

    def enable(self):
        """ Enables the class by finding the service and its characteristics. """
        global s_speaker_status_handle
        global s_microphone_handle

        if self.sound_service is None:
            self.sound_service = self.periph.getServiceByUUID(self.serviceUUID)
        if self.config_char is None:
            self.config_char = self.sound_service.getCharacteristics(self.config_char_uuid)[0]
        if self.speaker_data_char is None:
            self.speaker_data_char = self.sound_service.getCharacteristics(self.speaker_data_char_uuid)[0]
        if self.speaker_status_char is None:
            self.speaker_status_char = self.sound_service.getCharacteristics(self.speaker_status_char_uuid)[0]
            s_speaker_status_handle = self.speaker_status_char.getHandle()
            self.speaker_status_char_cccd = self.speaker_status_char.getDescriptors(forUUID=CCCD_UUID)[0]
        if self.microphone_char is None:
            self.microphone_char = self.sound_service.getCharacteristics(self.microphone_char_uuid)[0]
            s_microphone_handle = self.microphone_char.getHandle()
            self.microphone_char_cccd = self.microphone_char.getDescriptors(forUUID=CCCD_UUID)[0]

    def play_speaker_sample(self, sample=0):
        if self.speaker_data_char is not None:
            sample_str = "{:02X}".format(sample)
            self.speaker_data_char.write(binascii.a2b_hex(sample_str), False)

    def set_speaker_status_notification(self, state):
        if self.speaker_status_char_cccd is not None:
            if state == True:
                self.speaker_status_char_cccd.write(b"\x01\x00", True)
            else:
                self.speaker_status_char_cccd.write(b"\x00\x00", True)

    def set_microphone_notification(self, state):
        if self.microphone_char_cccd is not None:
            if state == True:
                self.microphone_char_cccd.write(b"\x01\x00", True)
            else:
                self.microphone_char_cccd.write(b"\x00\x00", True)

    def configure(self, speaker_mode=None, microphone_mode=None):
        if speaker_mode is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint8(current_config, speaker_mode, 0)
            self.config_char.write(binascii.a2b_hex(new_config), True)
        if microphone_mode is not None and self.config_char is not None:
            current_config = binascii.b2a_hex(self.config_char.read())
            new_config = write_uint8(current_config, microphone_mode, 1)
            self.config_char.write(binascii.a2b_hex(new_config), True)

    def disable(self):
        self.set_speaker_status_notification(False)
        self.set_microphone_notification(False)


class MyDelegate(DefaultDelegate):
    
    def handleNotification(self, hnd, data):
        #Debug print repr(data)
        if (hnd == e_temperature_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Temp received:  {}.{} degCelsius'.format(
                        self._str_to_int(teptep[:-2]), int(teptep[-2:], 16)))
            
        elif (hnd == e_pressure_handle):
            pressure_int, pressure_dec = self._extract_pressure_data(data)
            print('Notification: Press received: {}.{} hPa'.format(
                        pressure_int, pressure_dec))

        elif (hnd == e_humidity_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Humidity received: {} %'.format(self._str_to_int(teptep)))

        elif (hnd == e_gas_handle):
            eco2, tvoc = self._extract_gas_data(data)
            print('Notification: Gas received: eCO2 ppm: {}, TVOC ppb: {} %'.format(eco2, tvoc))

        elif (hnd == e_color_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Color: {}'.format(teptep))            

        elif (hnd == ui_button_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Button state [1 -> released]: {}'.format(self._str_to_int(teptep)))

        elif (hnd == m_tap_handle):
            direction, count = self._extract_tap_data(data)
            print('Notification: Tap: direction: {}, count: {}'.format(direction, self._str_to_int(count)))

        elif (hnd == m_orient_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Orient: {}'.format(teptep))

        elif (hnd == m_quaternion_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Quaternion: {}'.format(teptep))

        elif (hnd == m_stepcnt_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Step Count: {}'.format(teptep))

        elif (hnd == m_rawdata_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Raw data: {}'.format(teptep))

        elif (hnd == m_euler_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Euler: {}'.format(teptep))

        elif (hnd == m_rotation_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Rotation matrix: {}'.format(teptep))

        elif (hnd == m_heading_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Heading: {}'.format(teptep))

        elif (hnd == m_gravity_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Gravity: {}'.format(teptep))        

        elif (hnd == s_speaker_status_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Speaker Status: {}'.format(teptep))

        elif (hnd == s_microphone_handle):
            teptep = binascii.b2a_hex(data)
            print('Notification: Microphone: {}'.format(teptep))

        else:
            teptep = binascii.b2a_hex(data)
            print('Notification: UNKOWN: hnd {}, data {}'.format(hnd, teptep))
            

    def _str_to_int(self, s):
        """ Transform hex str into int. """
        i = int(s, 16)
        if i >= 2**7:
            i -= 2**8
        return i    

    def _extract_pressure_data(self, data):
        """ Extract pressure data from data string. """
        teptep = binascii.b2a_hex(data)
        pressure_int = 0
        for i in range(0, 4):
                pressure_int += (int(teptep[i*2:(i*2)+2], 16) << 8*i)
        pressure_dec = int(teptep[-2:], 16)
        return (pressure_int, pressure_dec)

    def _extract_gas_data(self, data):
        """ Extract gas data from data string. """
        teptep = binascii.b2a_hex(data)
        eco2 = int(teptep[:2], 16) + (int(teptep[2:4], 16) << 8)
        tvoc = int(teptep[4:6], 16) + (int(teptep[6:8], 16) << 8)
        return eco2, tvoc

    def _extract_tap_data(self, data):
        """ Extract tap data from data string. """
        teptep = binascii.b2a_hex(data)
        direction = teptep[0:2]
        count = teptep[2:4]
        return (direction, count)


class Thingy52(Peripheral):
    """
    Thingy:52 module. Instance the class and enable to get access to the Thingy:52 Sensors.
    The addr of your device has to be know, or can be found by using the hcitool command line 
    tool, for example. Call "> sudo hcitool lescan" and your Thingy's address should show up.
    """
    def __init__(self, addr):
        Peripheral.__init__(self, addr, addrType=ADDR_TYPE_RANDOM)

        # Thingy configuration service not implemented
        self.battery = BatterySensor(self)
        self.environment = EnvironmentService(self)
        self.ui = UserInterfaceService(self)
        self.motion = MotionService(self)
        self.sound = SoundService(self)
        # DFU Service not implemented


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mac_address', action='store', help='MAC address of BLE peripheral')
    parser.add_argument('-n', action='store', dest='count', default=0,
                            type=int, help="Number of times to loop data")
    parser.add_argument('-t',action='store',type=float, default=2.0, help='time between polling')
    parser.add_argument('--temperature', action="store_true",default=False)
    parser.add_argument('--pressure', action="store_true",default=False)
    parser.add_argument('--humidity', action="store_true",default=False)
    parser.add_argument('--gas', action="store_true",default=False)
    parser.add_argument('--color', action="store_true",default=False)
    parser.add_argument('--keypress', action='store_true', default=False)
    parser.add_argument('--tap', action='store_true', default=False)
    parser.add_argument('--orientation', action='store_true', default=False)
    parser.add_argument('--quaternion', action='store_true', default=False)
    parser.add_argument('--stepcnt', action='store_true', default=False)
    parser.add_argument('--rawdata', action='store_true', default=False)
    parser.add_argument('--euler', action='store_true', default=False)
    parser.add_argument('--rotation', action='store_true', default=False)
    parser.add_argument('--heading', action='store_true', default=False)
    parser.add_argument('--gravity', action='store_true', default=False)
    parser.add_argument('--battery', action='store_true', default=False)
    parser.add_argument('--speaker', action='store_true', default=False)
    parser.add_argument('--microphone', action='store_true', default=False)
    args = parser.parse_args()

    print('Connecting to ' + args.mac_address)
    thingy = Thingy52(args.mac_address)
    print('Connected...')
    thingy.setDelegate(MyDelegate())

    try:
        # Set LED so that we know we are connected
        thingy.ui.enable()
        thingy.ui.set_led_mode_breathe(0x01, 50, 100) # 0x01 = RED
        print('LED set to breathe mode...')

        # Enabling selected sensors
        print('Enabling selected sensors...')
        # Environment Service
        if args.temperature:
            thingy.environment.enable()
            thingy.environment.configure(temp_int=1000)
            thingy.environment.set_temperature_notification(True)
        if args.pressure:
            thingy.environment.enable()
            thingy.environment.configure(press_int=1000)
            thingy.environment.set_pressure_notification(True)
        if args.humidity:
            thingy.environment.enable()
            thingy.environment.configure(humid_int=1000)
            thingy.environment.set_humidity_notification(True)
        if args.gas:
            thingy.environment.enable()
            thingy.environment.configure(gas_mode_int=1)
            thingy.environment.set_gas_notification(True)
        if args.color:
            thingy.environment.enable()
            thingy.environment.configure(color_int=1000)
            thingy.environment.configure(color_sens_calib=[0,0,0])
            thingy.environment.set_color_notification(True)
        # User Interface Service
        if args.keypress:
            thingy.ui.enable()
            thingy.ui.set_btn_notification(True)
        if args.battery:
            thingy.battery.enable()
        # Motion Service
        if args.tap:
            thingy.motion.enable()
            thingy.motion.configure(motion_freq=200)
            thingy.motion.set_tap_notification(True)
        if args.orientation:
            thingy.motion.enable()
            thingy.motion.set_orient_notification(True)
        if args.quaternion:
            thingy.motion.enable()
            thingy.motion.set_quaternion_notification(True)
        if args.stepcnt:
            thingy.motion.enable()
            thingy.motion.configure(step_int=100)
            thingy.motion.set_stepcnt_notification(True)
        if args.rawdata:
            thingy.motion.enable()
            thingy.motion.set_rawdata_notification(True)
        if args.euler:
            thingy.motion.enable()
            thingy.motion.set_euler_notification(True)
        if args.rotation:
            thingy.motion.enable()
            thingy.motion.set_rotation_notification(True)
        if args.heading:
            thingy.motion.enable()
            thingy.motion.set_heading_notification(True)
        if args.gravity:
            thingy.motion.enable()
            thingy.motion.set_gravity_notification(True)
        # Sound Service
        if args.speaker:
            thingy.sound.enable()
            thingy.sound.configure(speaker_mode=0x03)
            thingy.sound.set_speaker_status_notification(True)
            # Test speaker
            thingy.sound.play_speaker_sample(1)
        if args.microphone:
            thingy.sound.enable()
            thingy.sound.configure(microphone_mode=0x01)
            thingy.sound.set_microphone_notification(True)

        # Allow sensors time to start up (might need more time for some sensors to be ready)
        print('All requested sensors and notifications are enabled...')
        time.sleep(1.0)
        
        counter=1
        while True:
            if args.battery:
                print("Battery: ", thingy.battery.read())

            if counter >= args.count:
                break
            
            counter += 1
            thingy.waitForNotifications(args.t)

    finally:
        thingy.disconnect()
        del thingy


if __name__ == "__main__":
    main()
