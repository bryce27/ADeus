#!/usr/bin/env python3
"""
ADeus BLE GATT Server for Raspberry Pi Zero W
Works with Chrome Web Bluetooth API

Requires: sudo apt install python3-dbus python3-gi
Run with: sudo python3 ble_gatt_server.py
"""

import dbus
import dbus.exceptions
import dbus.mainloop.glib
import dbus.service
import array
import struct
import time
import threading
import subprocess
import sys
import os
import signal

try:
    from gi.repository import GLib
except ImportError:
    import glib as GLib

BLUEZ_SERVICE_NAME = 'org.bluez'
GATT_MANAGER_IFACE = 'org.bluez.GattManager1'
LE_ADVERTISING_MANAGER_IFACE = 'org.bluez.LEAdvertisingManager1'
DBUS_OM_IFACE = 'org.freedesktop.DBus.ObjectManager'
DBUS_PROP_IFACE = 'org.freedesktop.DBus.Properties'

GATT_SERVICE_IFACE = 'org.bluez.GattService1'
GATT_CHRC_IFACE = 'org.bluez.GattCharacteristic1'
GATT_DESC_IFACE = 'org.bluez.GattDescriptor1'
LE_ADVERTISEMENT_IFACE = 'org.bluez.LEAdvertisement1'

# ADeus UUIDs (must match the app)
ADEUS_SERVICE_UUID = '4fafc201-1fb5-459e-8fcc-c5c9c331914b'
AUDIO_CHAR_UUID = 'beb5483e-36e1-4688-b7f5-ea07361b26a8'
CONTROL_CHAR_UUID = 'beb5483e-36e1-4688-b7f5-ea07361b26a9'
STATUS_CHAR_UUID = 'beb5483e-36e1-4688-b7f5-ea07361b26aa'


class InvalidArgsException(dbus.exceptions.DBusException):
    _dbus_error_name = 'org.freedesktop.DBus.Error.InvalidArgs'


class NotSupportedException(dbus.exceptions.DBusException):
    _dbus_error_name = 'org.bluez.Error.NotSupported'


class NotPermittedException(dbus.exceptions.DBusException):
    _dbus_error_name = 'org.bluez.Error.NotPermitted'


class Advertisement(dbus.service.Object):
    PATH_BASE = '/org/bluez/adeus/advertisement'

    def __init__(self, bus, index, advertising_type):
        self.path = self.PATH_BASE + str(index)
        self.bus = bus
        self.ad_type = advertising_type
        self.service_uuids = None
        self.manufacturer_data = None
        self.solicit_uuids = None
        self.service_data = None
        self.local_name = None
        self.include_tx_power = False
        dbus.service.Object.__init__(self, bus, self.path)

    def get_properties(self):
        properties = dict()
        properties['Type'] = self.ad_type
        if self.service_uuids is not None:
            properties['ServiceUUIDs'] = dbus.Array(self.service_uuids, signature='s')
        if self.solicit_uuids is not None:
            properties['SolicitUUIDs'] = dbus.Array(self.solicit_uuids, signature='s')
        if self.manufacturer_data is not None:
            properties['ManufacturerData'] = dbus.Dictionary(self.manufacturer_data, signature='qv')
        if self.service_data is not None:
            properties['ServiceData'] = dbus.Dictionary(self.service_data, signature='sv')
        if self.local_name is not None:
            properties['LocalName'] = dbus.String(self.local_name)
        if self.include_tx_power:
            properties['Includes'] = dbus.Array(["tx-power"], signature='s')
        return {LE_ADVERTISEMENT_IFACE: properties}

    def get_path(self):
        return dbus.ObjectPath(self.path)

    @dbus.service.method(DBUS_PROP_IFACE, in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        if interface != LE_ADVERTISEMENT_IFACE:
            raise InvalidArgsException()
        return self.get_properties()[LE_ADVERTISEMENT_IFACE]

    @dbus.service.method(LE_ADVERTISEMENT_IFACE, in_signature='', out_signature='')
    def Release(self):
        print('Advertisement released')


class ADeusAdvertisement(Advertisement):
    def __init__(self, bus, index):
        Advertisement.__init__(self, bus, index, 'peripheral')
        self.service_uuids = [ADEUS_SERVICE_UUID]
        self.local_name = 'ADeus-Pi'
        self.include_tx_power = True


class Application(dbus.service.Object):
    def __init__(self, bus):
        self.path = '/org/bluez/adeus'
        self.services = []
        dbus.service.Object.__init__(self, bus, self.path)

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def add_service(self, service):
        self.services.append(service)

    @dbus.service.method(DBUS_OM_IFACE, out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        response = {}
        for service in self.services:
            response[service.get_path()] = service.get_properties()
            chrcs = service.get_characteristics()
            for chrc in chrcs:
                response[chrc.get_path()] = chrc.get_properties()
                descs = chrc.get_descriptors()
                for desc in descs:
                    response[desc.get_path()] = desc.get_properties()
        return response


class Service(dbus.service.Object):
    PATH_BASE = '/org/bluez/adeus/service'

    def __init__(self, bus, index, uuid, primary):
        self.path = self.PATH_BASE + str(index)
        self.bus = bus
        self.uuid = uuid
        self.primary = primary
        self.characteristics = []
        dbus.service.Object.__init__(self, bus, self.path)

    def get_properties(self):
        return {
            GATT_SERVICE_IFACE: {
                'UUID': self.uuid,
                'Primary': self.primary,
                'Characteristics': dbus.Array(
                    self.get_characteristic_paths(),
                    signature='o')
            }
        }

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def add_characteristic(self, characteristic):
        self.characteristics.append(characteristic)

    def get_characteristic_paths(self):
        result = []
        for chrc in self.characteristics:
            result.append(chrc.get_path())
        return result

    def get_characteristics(self):
        return self.characteristics

    @dbus.service.method(DBUS_PROP_IFACE, in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        if interface != GATT_SERVICE_IFACE:
            raise InvalidArgsException()
        return self.get_properties()[GATT_SERVICE_IFACE]


class Characteristic(dbus.service.Object):
    def __init__(self, bus, index, uuid, flags, service):
        self.path = service.path + '/char' + str(index)
        self.bus = bus
        self.uuid = uuid
        self.service = service
        self.flags = flags
        self.descriptors = []
        dbus.service.Object.__init__(self, bus, self.path)

    def get_properties(self):
        return {
            GATT_CHRC_IFACE: {
                'Service': self.service.get_path(),
                'UUID': self.uuid,
                'Flags': self.flags,
                'Descriptors': dbus.Array(self.get_descriptor_paths(), signature='o')
            }
        }

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def add_descriptor(self, descriptor):
        self.descriptors.append(descriptor)

    def get_descriptor_paths(self):
        result = []
        for desc in self.descriptors:
            result.append(desc.get_path())
        return result

    def get_descriptors(self):
        return self.descriptors

    @dbus.service.method(DBUS_PROP_IFACE, in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        if interface != GATT_CHRC_IFACE:
            raise InvalidArgsException()
        return self.get_properties()[GATT_CHRC_IFACE]

    @dbus.service.method(GATT_CHRC_IFACE, in_signature='a{sv}', out_signature='ay')
    def ReadValue(self, options):
        print('Default ReadValue called, returning error')
        raise NotSupportedException()

    @dbus.service.method(GATT_CHRC_IFACE, in_signature='aya{sv}')
    def WriteValue(self, value, options):
        print('Default WriteValue called, returning error')
        raise NotSupportedException()

    @dbus.service.method(GATT_CHRC_IFACE)
    def StartNotify(self):
        print('Default StartNotify called, returning error')
        raise NotSupportedException()

    @dbus.service.method(GATT_CHRC_IFACE)
    def StopNotify(self):
        print('Default StopNotify called, returning error')
        raise NotSupportedException()

    @dbus.service.signal(DBUS_PROP_IFACE, signature='sa{sv}as')
    def PropertiesChanged(self, interface, changed, invalidated):
        pass


class Descriptor(dbus.service.Object):
    def __init__(self, bus, index, uuid, flags, characteristic):
        self.path = characteristic.path + '/desc' + str(index)
        self.bus = bus
        self.uuid = uuid
        self.flags = flags
        self.chrc = characteristic
        dbus.service.Object.__init__(self, bus, self.path)

    def get_properties(self):
        return {
            GATT_DESC_IFACE: {
                'Characteristic': self.chrc.get_path(),
                'UUID': self.uuid,
                'Flags': self.flags,
            }
        }

    def get_path(self):
        return dbus.ObjectPath(self.path)

    @dbus.service.method(DBUS_PROP_IFACE, in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        if interface != GATT_DESC_IFACE:
            raise InvalidArgsException()
        return self.get_properties()[GATT_DESC_IFACE]

    @dbus.service.method(GATT_DESC_IFACE, in_signature='a{sv}', out_signature='ay')
    def ReadValue(self, options):
        print('Default ReadValue called, returning error')
        raise NotSupportedException()

    @dbus.service.method(GATT_DESC_IFACE, in_signature='aya{sv}')
    def WriteValue(self, value, options):
        print('Default WriteValue called, returning error')
        raise NotSupportedException()


# ADeus-specific implementations

class ADeusService(Service):
    def __init__(self, bus, index):
        Service.__init__(self, bus, index, ADEUS_SERVICE_UUID, True)
        self.add_characteristic(AudioCharacteristic(bus, 0, self))
        self.add_characteristic(ControlCharacteristic(bus, 1, self))
        self.add_characteristic(StatusCharacteristic(bus, 2, self))


class AudioCharacteristic(Characteristic):
    """Audio data characteristic - supports notify for streaming audio to phone"""
    
    def __init__(self, bus, index, service):
        Characteristic.__init__(
            self, bus, index, AUDIO_CHAR_UUID,
            ['notify'], service)
        self.notifying = False
        self.audio_queue = []
        self.lock = threading.Lock()

    def queue_audio(self, data):
        """Queue audio data to be sent via notifications"""
        with self.lock:
            self.audio_queue.append(data)
        if self.notifying:
            GLib.idle_add(self.send_queued_audio)

    def send_queued_audio(self):
        """Send queued audio data as notifications"""
        with self.lock:
            if not self.audio_queue:
                return False
            data = self.audio_queue.pop(0)
        
        # Send in chunks (max BLE MTU is typically 512 bytes, but 185 is safer)
        chunk_size = 185
        for i in range(0, len(data), chunk_size):
            chunk = data[i:i + chunk_size]
            value = dbus.Array([dbus.Byte(b) for b in chunk], signature='y')
            self.PropertiesChanged(GATT_CHRC_IFACE, {'Value': value}, [])
            time.sleep(0.004)  # Small delay between chunks
        
        return False

    def StartNotify(self):
        if self.notifying:
            return
        self.notifying = True
        print('Audio notifications enabled')

    def StopNotify(self):
        if not self.notifying:
            return
        self.notifying = False
        print('Audio notifications disabled')


class ControlCharacteristic(Characteristic):
    """Control characteristic - phone writes commands here"""
    
    # Command codes
    CMD_START_RECORDING = 0x01
    CMD_STOP_RECORDING = 0x02
    CMD_PAUSE_RECORDING = 0x03
    CMD_RESUME_RECORDING = 0x04
    CMD_SET_GAIN = 0x10
    CMD_SET_SAMPLE_RATE = 0x11
    CMD_SET_CHUNK_DURATION = 0x12
    CMD_GET_STATUS = 0x20
    CMD_PING = 0xFF
    
    def __init__(self, bus, index, service):
        Characteristic.__init__(
            self, bus, index, CONTROL_CHAR_UUID,
            ['write', 'write-without-response'], service)
        self.command_callback = None

    def set_command_callback(self, callback):
        self.command_callback = callback

    def WriteValue(self, value, options):
        data = bytes(value)
        if len(data) < 1:
            return
        
        cmd = data[0]
        print(f'Received command: 0x{cmd:02X}')
        
        if self.command_callback:
            self.command_callback(cmd, data)
        
        # Handle built-in commands
        if cmd == self.CMD_PING:
            print('PING received')
        elif cmd == self.CMD_START_RECORDING:
            print('START_RECORDING command')
        elif cmd == self.CMD_STOP_RECORDING:
            print('STOP_RECORDING command')
        elif cmd == self.CMD_GET_STATUS:
            print('GET_STATUS command')


class StatusCharacteristic(Characteristic):
    """Status characteristic - phone reads current status"""
    
    def __init__(self, bus, index, service):
        Characteristic.__init__(
            self, bus, index, STATUS_CHAR_UUID,
            ['read', 'notify'], service)
        self.notifying = False
        self.status = {
            'state': 0,  # 0=stopped, 1=recording, 2=paused
            'battery': 255,  # 255 = unknown
            'gain': 1.0,
            'sample_rate': 44100,
            'chunk_duration': 60,
            'bytes_recorded': 0,
            'uptime': 0
        }

    def update_status(self, **kwargs):
        self.status.update(kwargs)
        if self.notifying:
            GLib.idle_add(self.notify_status)

    def notify_status(self):
        value = self.build_status_packet()
        self.PropertiesChanged(GATT_CHRC_IFACE, {'Value': value}, [])
        return False

    def build_status_packet(self):
        # Pack status into bytes matching the C++ StatusPacket structure
        # Magic: 'A', 'S'
        data = bytearray([ord('A'), ord('S')])
        data.append(self.status['state'])
        data.append(self.status['battery'])
        data.extend(struct.pack('<f', self.status['gain']))
        data.extend(struct.pack('<I', self.status['sample_rate']))
        data.extend(struct.pack('<H', self.status['chunk_duration']))
        data.extend(struct.pack('<I', self.status['bytes_recorded']))
        data.extend(struct.pack('<I', self.status['uptime']))
        return dbus.Array([dbus.Byte(b) for b in data], signature='y')

    def ReadValue(self, options):
        print('Status read requested')
        return self.build_status_packet()

    def StartNotify(self):
        if self.notifying:
            return
        self.notifying = True
        print('Status notifications enabled')

    def StopNotify(self):
        if not self.notifying:
            return
        self.notifying = False
        print('Status notifications disabled')


def find_adapter(bus):
    """Find the Bluetooth adapter"""
    remote_om = dbus.Interface(bus.get_object(BLUEZ_SERVICE_NAME, '/'),
                               DBUS_OM_IFACE)
    objects = remote_om.GetManagedObjects()

    for o, props in objects.items():
        if GATT_MANAGER_IFACE in props.keys():
            return o
    return None


def register_app_cb():
    print('GATT application registered successfully')


def register_app_error_cb(error):
    print(f'Failed to register application: {error}')
    mainloop.quit()


def register_ad_cb():
    print('Advertisement registered successfully')


def register_ad_error_cb(error):
    print(f'Failed to register advertisement: {error}')
    mainloop.quit()


mainloop = None


def main():
    global mainloop

    # Make sure bluetooth service is running
    subprocess.run(['sudo', 'systemctl', 'start', 'bluetooth'], capture_output=True)
    time.sleep(1)
    
    # Power on adapter
    subprocess.run(['sudo', 'hciconfig', 'hci0', 'up'], capture_output=True)
    time.sleep(0.5)

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    adapter = find_adapter(bus)
    if not adapter:
        print('ERROR: Bluetooth adapter not found')
        print('Make sure bluetooth service is running: sudo systemctl start bluetooth')
        return 1

    print(f'Using adapter: {adapter}')

    # Get adapter properties and enable LE
    adapter_props = dbus.Interface(
        bus.get_object(BLUEZ_SERVICE_NAME, adapter),
        DBUS_PROP_IFACE)
    
    try:
        adapter_props.Set('org.bluez.Adapter1', 'Powered', dbus.Boolean(True))
    except:
        pass

    # Get service managers
    service_manager = dbus.Interface(
        bus.get_object(BLUEZ_SERVICE_NAME, adapter),
        GATT_MANAGER_IFACE)

    ad_manager = dbus.Interface(
        bus.get_object(BLUEZ_SERVICE_NAME, adapter),
        LE_ADVERTISING_MANAGER_IFACE)

    # Create and register the application
    app = Application(bus)
    adeus_service = ADeusService(bus, 0)
    app.add_service(adeus_service)

    # Create advertisement
    advertisement = ADeusAdvertisement(bus, 0)

    mainloop = GLib.MainLoop()

    # Register GATT application
    print('Registering GATT application...')
    service_manager.RegisterApplication(
        app.get_path(), {},
        reply_handler=register_app_cb,
        error_handler=register_app_error_cb)

    # Register advertisement
    print('Registering advertisement...')
    ad_manager.RegisterAdvertisement(
        advertisement.get_path(), {},
        reply_handler=register_ad_cb,
        error_handler=register_ad_error_cb)

    print('')
    print('=' * 50)
    print('ADeus BLE GATT Server Started!')
    print('=' * 50)
    print(f'Device name: ADeus-Pi')
    print(f'Service UUID: {ADEUS_SERVICE_UUID}')
    print('')
    print('Characteristics:')
    print(f'  Audio (notify):  {AUDIO_CHAR_UUID}')
    print(f'  Control (write): {CONTROL_CHAR_UUID}')
    print(f'  Status (read):   {STATUS_CHAR_UUID}')
    print('')
    print('Waiting for connections from Chrome...')
    print('Press Ctrl+C to stop')
    print('')

    def signal_handler(sig, frame):
        print('\nShutting down...')
        mainloop.quit()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    try:
        mainloop.run()
    except KeyboardInterrupt:
        pass

    print('Server stopped')
    return 0


if __name__ == '__main__':
    sys.exit(main())
