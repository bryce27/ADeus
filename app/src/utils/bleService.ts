/**
 * ADeus BLE Service
 * 
 * Handles Bluetooth LE communication with ADeus Pi devices.
 * Supports audio streaming, remote control, and status monitoring.
 */

// BLE UUIDs (must match Pi's ble_protocol.h)
export const BLE_SERVICE_UUID = '4fafc201-1fb5-459e-8fcc-c5c9c331914b';
export const BLE_AUDIO_CHAR_UUID = 'beb5483e-36e1-4688-b7f5-ea07361b26a8';
export const BLE_CONTROL_CHAR_UUID = 'beb5483e-36e1-4688-b7f5-ea07361b26a9';
export const BLE_STATUS_CHAR_UUID = 'beb5483e-36e1-4688-b7f5-ea07361b26aa';

// Commands (must match Pi's ble_protocol.h)
export enum Command {
  START_RECORDING = 0x01,
  STOP_RECORDING = 0x02,
  PAUSE_RECORDING = 0x03,
  RESUME_RECORDING = 0x04,
  SET_GAIN = 0x10,
  SET_SAMPLE_RATE = 0x11,
  SET_CHUNK_DURATION = 0x12,
  GET_STATUS = 0x20,
  PING = 0xff,
}

export enum RecordingState {
  STOPPED = 0x00,
  RECORDING = 0x01,
  PAUSED = 0x02,
  ERROR = 0xff,
}

export interface DeviceStatus {
  state: RecordingState;
  batteryPercent: number;
  currentGain: number;
  sampleRate: number;
  chunkDuration: number;
  bytesRecorded: number;
  uptime: number;
}

export interface BLEDevice {
  id: string;
  name: string;
  rssi?: number;
}

type StatusCallback = (status: DeviceStatus) => void;
type AudioCallback = (data: ArrayBuffer, seqNum: number) => void;
type ConnectionCallback = (connected: boolean) => void;

class BLEService {
  private device: BluetoothDevice | null = null;
  private server: BluetoothRemoteGATTServer | null = null;
  private audioChar: BluetoothRemoteGATTCharacteristic | null = null;
  private controlChar: BluetoothRemoteGATTCharacteristic | null = null;
  private statusChar: BluetoothRemoteGATTCharacteristic | null = null;

  private statusCallbacks: Set<StatusCallback> = new Set();
  private audioCallbacks: Set<AudioCallback> = new Set();
  private connectionCallbacks: Set<ConnectionCallback> = new Set();

  private audioBuffer: ArrayBuffer[] = [];
  private expectedPackets = 0;
  private receivedPackets = 0;

  // Check if Web Bluetooth is available
  isAvailable(): boolean {
    return 'bluetooth' in navigator;
  }

  // Check if we're connected
  isConnected(): boolean {
    return this.server?.connected ?? false;
  }

  // Scan for ADeus devices
  async scanForDevices(): Promise<BLEDevice | null> {
    if (!this.isAvailable()) {
      throw new Error('Bluetooth is not available on this device');
    }

    try {
      this.device = await navigator.bluetooth.requestDevice({
        filters: [
          { namePrefix: 'ADeus' },
          { services: [BLE_SERVICE_UUID] },
        ],
        optionalServices: [BLE_SERVICE_UUID],
      });

      if (this.device) {
        // Listen for disconnection
        this.device.addEventListener('gattserverdisconnected', () => {
          console.log('Device disconnected');
          this.notifyConnectionChange(false);
        });

        return {
          id: this.device.id,
          name: this.device.name || 'Unknown ADeus Device',
        };
      }
    } catch (error) {
      console.error('Scan failed:', error);
      throw error;
    }

    return null;
  }

  // Connect to a device
  async connect(): Promise<boolean> {
    if (!this.device) {
      throw new Error('No device selected. Call scanForDevices first.');
    }

    try {
      console.log('Connecting to device...');
      this.server = await this.device.gatt?.connect() ?? null;

      if (!this.server) {
        throw new Error('Failed to connect to GATT server');
      }

      console.log('Getting service...');
      const service = await this.server.getPrimaryService(BLE_SERVICE_UUID);

      console.log('Getting characteristics...');
      
      // Get audio characteristic (for receiving audio data)
      try {
        this.audioChar = await service.getCharacteristic(BLE_AUDIO_CHAR_UUID);
        await this.audioChar.startNotifications();
        this.audioChar.addEventListener('characteristicvaluechanged', (event) => {
          this.handleAudioData(event);
        });
      } catch (e) {
        console.warn('Audio characteristic not available:', e);
      }

      // Get control characteristic (for sending commands)
      try {
        this.controlChar = await service.getCharacteristic(BLE_CONTROL_CHAR_UUID);
      } catch (e) {
        console.warn('Control characteristic not available:', e);
      }

      // Get status characteristic
      try {
        this.statusChar = await service.getCharacteristic(BLE_STATUS_CHAR_UUID);
        await this.statusChar.startNotifications();
        this.statusChar.addEventListener('characteristicvaluechanged', (event) => {
          this.handleStatusUpdate(event);
        });
      } catch (e) {
        console.warn('Status characteristic not available:', e);
      }

      console.log('Connected successfully!');
      this.notifyConnectionChange(true);
      return true;
    } catch (error) {
      console.error('Connection failed:', error);
      this.notifyConnectionChange(false);
      throw error;
    }
  }

  // Disconnect from device
  async disconnect(): Promise<void> {
    if (this.server?.connected) {
      this.server.disconnect();
    }
    this.device = null;
    this.server = null;
    this.audioChar = null;
    this.controlChar = null;
    this.statusChar = null;
    this.notifyConnectionChange(false);
  }

  // Send a command to the device
  async sendCommand(command: Command, param?: number): Promise<void> {
    if (!this.controlChar) {
      throw new Error('Not connected or control not available');
    }

    let data: Uint8Array;

    if (param !== undefined) {
      // Command with parameter
      const buffer = new ArrayBuffer(5);
      const view = new DataView(buffer);
      view.setUint8(0, command);
      
      if (command === Command.SET_GAIN) {
        view.setFloat32(1, param, true);
      } else if (command === Command.SET_CHUNK_DURATION) {
        view.setUint16(1, param, true);
      } else {
        view.setUint32(1, param, true);
      }
      
      data = new Uint8Array(buffer);
    } else {
      // Simple command
      data = new Uint8Array([command]);
    }

    await this.controlChar.writeValue(data);
  }

  // Convenience methods for common commands
  async startRecording(): Promise<void> {
    await this.sendCommand(Command.START_RECORDING);
  }

  async stopRecording(): Promise<void> {
    await this.sendCommand(Command.STOP_RECORDING);
  }

  async pauseRecording(): Promise<void> {
    await this.sendCommand(Command.PAUSE_RECORDING);
  }

  async resumeRecording(): Promise<void> {
    await this.sendCommand(Command.RESUME_RECORDING);
  }

  async setGain(gain: number): Promise<void> {
    await this.sendCommand(Command.SET_GAIN, gain);
  }

  async setSampleRate(rate: number): Promise<void> {
    await this.sendCommand(Command.SET_SAMPLE_RATE, rate);
  }

  async setChunkDuration(seconds: number): Promise<void> {
    await this.sendCommand(Command.SET_CHUNK_DURATION, seconds);
  }

  async requestStatus(): Promise<void> {
    await this.sendCommand(Command.GET_STATUS);
  }

  // Subscribe to status updates
  onStatusUpdate(callback: StatusCallback): () => void {
    this.statusCallbacks.add(callback);
    return () => this.statusCallbacks.delete(callback);
  }

  // Subscribe to audio data
  onAudioData(callback: AudioCallback): () => void {
    this.audioCallbacks.add(callback);
    return () => this.audioCallbacks.delete(callback);
  }

  // Subscribe to connection changes
  onConnectionChange(callback: ConnectionCallback): () => void {
    this.connectionCallbacks.add(callback);
    return () => this.connectionCallbacks.delete(callback);
  }

  // Handle incoming audio data
  private handleAudioData(event: Event): void {
    const target = event.target as BluetoothRemoteGATTCharacteristic;
    const value = target.value;
    
    if (!value) return;

    // Check for audio packet header (magic bytes 'AD')
    if (value.byteLength >= 12 && value.getUint8(0) === 0x41 && value.getUint8(1) === 0x44) {
      const seqNum = value.getUint16(2, true);
      const packetSize = value.getUint16(4, true);
      const totalPackets = value.getUint16(6, true);
      
      if (seqNum === 0) {
        // Start of new audio chunk
        this.audioBuffer = [];
        this.expectedPackets = totalPackets;
        this.receivedPackets = 0;
      }
      
      // Audio data follows header
      if (value.byteLength > 12) {
        const audioData = value.buffer.slice(12);
        this.audioBuffer.push(audioData);
        this.receivedPackets++;
        
        // Notify listeners with partial data
        this.audioCallbacks.forEach(cb => cb(audioData, seqNum));
      }
    } else {
      // Raw audio data (continuation packet)
      this.audioBuffer.push(value.buffer);
      this.audioCallbacks.forEach(cb => cb(value.buffer, this.receivedPackets));
    }
  }

  // Handle incoming status updates
  private handleStatusUpdate(event: Event): void {
    const target = event.target as BluetoothRemoteGATTCharacteristic;
    const value = target.value;
    
    if (!value || value.byteLength < 20) return;

    // Check for status packet header (magic bytes 'AS')
    if (value.getUint8(0) === 0x41 && value.getUint8(1) === 0x53) {
      const status: DeviceStatus = {
        state: value.getUint8(2) as RecordingState,
        batteryPercent: value.getUint8(3),
        currentGain: value.getFloat32(4, true),
        sampleRate: value.getUint32(8, true),
        chunkDuration: value.getUint16(12, true),
        bytesRecorded: value.getUint32(14, true),
        uptime: value.getUint32(18, true),
      };

      this.statusCallbacks.forEach(cb => cb(status));
    }
  }

  private notifyConnectionChange(connected: boolean): void {
    this.connectionCallbacks.forEach(cb => cb(connected));
  }
}

// Export singleton instance
export const bleService = new BLEService();
