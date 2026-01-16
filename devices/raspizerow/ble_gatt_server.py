#!/usr/bin/env python3
"""
ADeus BLE GATT Server for Raspberry Pi Zero W
Works with Chrome Web Bluetooth API

This script:
1. Runs the C++ audio recorder (./main) as a subprocess
2. Monitors for new audio files
3. Streams audio to connected Chrome clients via BLE

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
import glob
from pathlib import Path
from datetime import datetime

try:
    from gi.repository import GLib
except ImportError:
    import glib as GLib

# Get the directory where this script is located
SCRIPT_DIR = Path(__file__).parent.absolute()
DATA_DIR = SCRIPT_DIR / 'data'
MAIN_BINARY = SCRIPT_DIR / 'build' / 'main'

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


class AudioRecorderManager:
    """Manages the C++ audio recorder subprocess and monitors for new audio files"""
    
    def __init__(self, audio_characteristic, status_characteristic):
        self.audio_char = audio_characteristic
        self.status_char = status_characteristic
        self.recorder_process = None
        self.monitor_thread = None
        self.running = False
        self.recording = False
        self.processed_files = set()
        self.bytes_recorded = 0
        self.start_time = time.time()
        
    def start(self):
        """Start the audio recorder and file monitor"""
        self.running = True
        self.start_time = time.time()
        
        # Create data directory if it doesn't exist
        DATA_DIR.mkdir(exist_ok=True)
        
        # Get list of existing files to ignore
        for f in DATA_DIR.glob('*.wav'):
            self.processed_files.add(str(f))
        
        # Start file monitor thread
        self.monitor_thread = threading.Thread(target=self._monitor_files, daemon=True)
        self.monitor_thread.start()
        
        # Start the recorder
        self.start_recording()
        
        print(f'AudioRecorderManager started, monitoring {DATA_DIR}')
        
    def stop(self):
        """Stop the recorder and monitor"""
        self.running = False
        self.stop_recording()
        
    def start_recording(self):
        """Start the C++ recorder subprocess"""
        if self.recorder_process is not None:
            return
            
        if not MAIN_BINARY.exists():
            print(f'ERROR: Recorder binary not found at {MAIN_BINARY}')
            print('Please compile first: ./compile.sh')
            return
            
        print('Starting audio recorder...')
        try:
            # Run the recorder without bluetooth (we handle BLE in Python)
            self.recorder_process = subprocess.Popen(
                [str(MAIN_BINARY), '--save', '--verbose'],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                cwd=str(SCRIPT_DIR)
            )
            self.recording = True
            self.status_char.update_status(state=1)  # Recording
            
            # Start a thread to log recorder output
            threading.Thread(target=self._log_recorder_output, daemon=True).start()
            
            print(f'Recorder started (PID: {self.recorder_process.pid})')
        except Exception as e:
            print(f'Failed to start recorder: {e}')
            
    def stop_recording(self):
        """Stop the C++ recorder subprocess"""
        if self.recorder_process is None:
            return
            
        print('Stopping audio recorder...')
        self.recorder_process.terminate()
        try:
            self.recorder_process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.recorder_process.kill()
        self.recorder_process = None
        self.recording = False
        self.status_char.update_status(state=0)  # Stopped
        print('Recorder stopped')
        
    def _log_recorder_output(self):
        """Log output from the recorder subprocess"""
        if self.recorder_process is None:
            return
        for line in self.recorder_process.stdout:
            if not self.running:
                break
            line = line.decode('utf-8', errors='replace').strip()
            if line:
                print(f'[recorder] {line}')
                
    def _monitor_files(self):
        """Monitor the data directory for new audio files"""
        while self.running:
            try:
                # Check for new WAV files
                for filepath in DATA_DIR.glob('*.wav'):
                    filepath_str = str(filepath)
                    if filepath_str not in self.processed_files:
                        # Wait a bit to ensure file is fully written
                        time.sleep(0.5)
                        self._process_audio_file(filepath)
                        self.processed_files.add(filepath_str)
            except Exception as e:
                print(f'Error monitoring files: {e}')
                
            time.sleep(1)  # Check every second
            
            # Update uptime in status
            uptime = int(time.time() - self.start_time)
            self.status_char.update_status(
                uptime=uptime,
                bytes_recorded=self.bytes_recorded
            )
            
    def _process_audio_file(self, filepath):
        """Process a new audio file and send via BLE"""
        try:
            file_size = filepath.stat().st_size
            print(f'New audio file: {filepath.name} ({file_size} bytes)')
            
            self.bytes_recorded += file_size
            
            # Read the file
            with open(filepath, 'rb') as f:
                audio_data = f.read()
                
            # Queue for BLE transmission
            self.audio_char.queue_audio(audio_data)
            print(f'Queued {len(audio_data)} bytes for BLE transmission')
            
        except Exception as e:
            print(f'Error processing audio file: {e}')


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

    print('')
    print('    _    ____')
    print('   / \\  |  _ \\  ___ _   _ ___')
    print('  / _ \\ | | | |/ _ \\ | | / __|')
    print(' / ___ \\| |_| |  __/ |_| \\__ \\')
    print('/_/   \\_\\____/ \\___|\\__,_|___/')
    print('')
    print('ADeus BLE GATT Server + Audio Recorder')
    print('=' * 45)
    print('')

    # Make sure bluetooth service is running
    print('Starting Bluetooth service...')
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

    # Get references to characteristics for the recorder manager
    audio_char = adeus_service.characteristics[0]  # AudioCharacteristic
    control_char = adeus_service.characteristics[1]  # ControlCharacteristic
    status_char = adeus_service.characteristics[2]  # StatusCharacteristic

    # Create the audio recorder manager
    recorder_manager = AudioRecorderManager(audio_char, status_char)

    # Set up control command handling
    def handle_command(cmd, data):
        if cmd == ControlCharacteristic.CMD_START_RECORDING:
            recorder_manager.start_recording()
        elif cmd == ControlCharacteristic.CMD_STOP_RECORDING:
            recorder_manager.stop_recording()
        elif cmd == ControlCharacteristic.CMD_PAUSE_RECORDING:
            # TODO: Implement pause
            pass
        elif cmd == ControlCharacteristic.CMD_RESUME_RECORDING:
            # TODO: Implement resume
            pass

    control_char.set_command_callback(handle_command)

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
    
    # Check if recorder binary exists
    if MAIN_BINARY.exists():
        print(f'Recorder binary: {MAIN_BINARY}')
    else:
        print(f'WARNING: Recorder binary not found at {MAIN_BINARY}')
        print('         Run ./compile.sh first to build the recorder')
    
    print('')
    print('Starting audio recorder and file monitor...')
    recorder_manager.start()
    
    print('')
    print('Waiting for BLE connections from Chrome...')
    print('Press Ctrl+C to stop')
    print('')

    def signal_handler(sig, frame):
        print('\nShutting down...')
        recorder_manager.stop()
        mainloop.quit()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    try:
        mainloop.run()
    except KeyboardInterrupt:
        pass

    recorder_manager.stop()
    print('Server stopped')
    return 0


if __name__ == '__main__':
    sys.exit(main())
