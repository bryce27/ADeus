/**
 * ADeus BLE Service
 * 
 * Handles Bluetooth LE communication with ADeus Pi devices.
 * Uses native Capacitor plugin on iOS/Android, Web Bluetooth on desktop.
 */

import { Capacitor } from '@capacitor/core';
import { BleClient, BleDevice, dataViewToText, numberToUUID } from '@capacitor-community/bluetooth-le';

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
  SET_VAD_ENABLED = 0x13,      // Voice Activity Detection
  SET_VAD_THRESHOLD = 0x14,   // VAD sensitivity
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
  vadEnabled?: boolean;
  vadThreshold?: number;
}

export interface ADeusBLEDevice {
  id: string;
  name: string;
  rssi?: number;
}

type StatusCallback = (status: DeviceStatus) => void;
type AudioCallback = (data: ArrayBuffer, seqNum: number) => void;
type ConnectionCallback = (connected: boolean) => void;

class BLEService {
  private deviceId: string | null = null;
  private connected = false;
  private isNative = false;

  // Web Bluetooth specific
  private webDevice: BluetoothDevice | null = null;
  private webServer: BluetoothRemoteGATTServer | null = null;

  private statusCallbacks: Set<StatusCallback> = new Set();
  private audioCallbacks: Set<AudioCallback> = new Set();
  private connectionCallbacks: Set<ConnectionCallback> = new Set();

  private audioBuffer: ArrayBuffer[] = [];
  private expectedPackets = 0;
  private receivedPackets = 0;

  constructor() {
    this.isNative = Capacitor.isNativePlatform();
  }

  // Check if BLE is available
  async isAvailable(): Promise<boolean> {
    if (this.isNative) {
      try {
        await BleClient.initialize();
        return true;
      } catch {
        return false;
      }
    } else {
      return 'bluetooth' in navigator;
    }
  }

  // Check if we're connected
  isConnected(): boolean {
    return this.connected;
  }

  // Scan for ADeus devices
  async scanForDevices(): Promise<ADeusBLEDevice | null> {
    if (this.isNative) {
      return this.scanNative();
    } else {
      return this.scanWeb();
    }
  }

  // Native (iOS/Android) scanning
  private async scanNative(): Promise<ADeusBLEDevice | null> {
    try {
      await BleClient.initialize();
      
      // Request permissions on Android
      if (Capacitor.getPlatform() === 'android') {
        await BleClient.requestLEScan(
          { services: [BLE_SERVICE_UUID] },
          () => {} // We'll use requestDevice instead
        );
        await BleClient.stopLEScan();
      }

      const device = await BleClient.requestDevice({
        services: [BLE_SERVICE_UUID],
        namePrefix: 'ADeus',
      });

      if (device) {
        this.deviceId = device.deviceId;
        return {
          id: device.deviceId,
          name: device.name || 'ADeus Device',
        };
      }
    } catch (error) {
      console.error('Native scan failed:', error);
      throw error;
    }
    return null;
  }

  // Web Bluetooth scanning
  private async scanWeb(): Promise<ADeusBLEDevice | null> {
    try {
      this.webDevice = await navigator.bluetooth.requestDevice({
        filters: [
          { namePrefix: 'ADeus' },
          { services: [BLE_SERVICE_UUID] },
        ],
        optionalServices: [BLE_SERVICE_UUID],
      });

      if (this.webDevice) {
        this.webDevice.addEventListener('gattserverdisconnected', () => {
          this.connected = false;
          this.notifyConnectionChange(false);
        });

        this.deviceId = this.webDevice.id;
        return {
          id: this.webDevice.id,
          name: this.webDevice.name || 'ADeus Device',
        };
      }
    } catch (error) {
      console.error('Web scan failed:', error);
      throw error;
    }
    return null;
  }

  // Connect to device
  async connect(): Promise<boolean> {
    if (!this.deviceId) {
      throw new Error('No device selected. Call scanForDevices first.');
    }

    if (this.isNative) {
      return this.connectNative();
    } else {
      return this.connectWeb();
    }
  }

  // Native connection
  private async connectNative(): Promise<boolean> {
    try {
      await BleClient.connect(this.deviceId!, (deviceId) => {
        console.log('Device disconnected:', deviceId);
        this.connected = false;
        this.notifyConnectionChange(false);
      });

      // Subscribe to audio characteristic
      try {
        await BleClient.startNotifications(
          this.deviceId!,
          BLE_SERVICE_UUID,
          BLE_AUDIO_CHAR_UUID,
          (value) => this.handleAudioDataNative(value)
        );
      } catch (e) {
        console.warn('Audio notifications not available:', e);
      }

      // Subscribe to status characteristic
      try {
        await BleClient.startNotifications(
          this.deviceId!,
          BLE_SERVICE_UUID,
          BLE_STATUS_CHAR_UUID,
          (value) => this.handleStatusUpdateNative(value)
        );
      } catch (e) {
        console.warn('Status notifications not available:', e);
      }

      this.connected = true;
      this.notifyConnectionChange(true);
      return true;
    } catch (error) {
      console.error('Native connection failed:', error);
      throw error;
    }
  }

  // Web connection
  private async connectWeb(): Promise<boolean> {
    if (!this.webDevice) {
      throw new Error('No web device available');
    }

    try {
      this.webServer = await this.webDevice.gatt?.connect() ?? null;
      if (!this.webServer) {
        throw new Error('Failed to connect to GATT server');
      }

      const service = await this.webServer.getPrimaryService(BLE_SERVICE_UUID);

      // Audio characteristic
      try {
        const audioChar = await service.getCharacteristic(BLE_AUDIO_CHAR_UUID);
        await audioChar.startNotifications();
        audioChar.addEventListener('characteristicvaluechanged', (e) => {
          this.handleAudioDataWeb(e);
        });
      } catch (e) {
        console.warn('Audio characteristic not available:', e);
      }

      // Status characteristic
      try {
        const statusChar = await service.getCharacteristic(BLE_STATUS_CHAR_UUID);
        await statusChar.startNotifications();
        statusChar.addEventListener('characteristicvaluechanged', (e) => {
          this.handleStatusUpdateWeb(e);
        });
      } catch (e) {
        console.warn('Status characteristic not available:', e);
      }

      this.connected = true;
      this.notifyConnectionChange(true);
      return true;
    } catch (error) {
      console.error('Web connection failed:', error);
      throw error;
    }
  }

  // Disconnect
  async disconnect(): Promise<void> {
    if (this.isNative && this.deviceId) {
      try {
        await BleClient.disconnect(this.deviceId);
      } catch (e) {
        console.warn('Disconnect error:', e);
      }
    } else if (this.webServer?.connected) {
      this.webServer.disconnect();
    }

    this.connected = false;
    this.deviceId = null;
    this.webDevice = null;
    this.webServer = null;
    this.notifyConnectionChange(false);
  }

  // Send command
  async sendCommand(command: Command, param?: number): Promise<void> {
    if (!this.deviceId || !this.connected) {
      throw new Error('Not connected');
    }

    let data: DataView;
    
    if (param !== undefined) {
      const buffer = new ArrayBuffer(5);
      data = new DataView(buffer);
      data.setUint8(0, command);
      
      if (command === Command.SET_GAIN || command === Command.SET_VAD_THRESHOLD) {
        data.setFloat32(1, param, true);
      } else if (command === Command.SET_CHUNK_DURATION) {
        data.setUint16(1, param, true);
      } else if (command === Command.SET_VAD_ENABLED) {
        data.setUint8(1, param ? 1 : 0);
      } else {
        data.setUint32(1, param, true);
      }
    } else {
      const buffer = new ArrayBuffer(1);
      data = new DataView(buffer);
      data.setUint8(0, command);
    }

    if (this.isNative) {
      await BleClient.write(
        this.deviceId,
        BLE_SERVICE_UUID,
        BLE_CONTROL_CHAR_UUID,
        data
      );
    } else {
      const service = await this.webServer!.getPrimaryService(BLE_SERVICE_UUID);
      const controlChar = await service.getCharacteristic(BLE_CONTROL_CHAR_UUID);
      await controlChar.writeValue(data);
    }
  }

  // Convenience methods
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

  async setVADEnabled(enabled: boolean): Promise<void> {
    await this.sendCommand(Command.SET_VAD_ENABLED, enabled ? 1 : 0);
  }

  async setVADThreshold(threshold: number): Promise<void> {
    await this.sendCommand(Command.SET_VAD_THRESHOLD, threshold);
  }

  async requestStatus(): Promise<void> {
    await this.sendCommand(Command.GET_STATUS);
  }

  // Callbacks
  onStatusUpdate(callback: StatusCallback): () => void {
    this.statusCallbacks.add(callback);
    return () => this.statusCallbacks.delete(callback);
  }

  onAudioData(callback: AudioCallback): () => void {
    this.audioCallbacks.add(callback);
    return () => this.audioCallbacks.delete(callback);
  }

  onConnectionChange(callback: ConnectionCallback): () => void {
    this.connectionCallbacks.add(callback);
    return () => this.connectionCallbacks.delete(callback);
  }

  // Handle native audio data
  private handleAudioDataNative(value: DataView): void {
    if (value.byteLength >= 12 && value.getUint8(0) === 0x41 && value.getUint8(1) === 0x44) {
      const seqNum = value.getUint16(2, true);
      const audioData = value.buffer.slice(12);
      this.audioCallbacks.forEach(cb => cb(audioData, seqNum));
    } else {
      this.audioCallbacks.forEach(cb => cb(value.buffer, this.receivedPackets++));
    }
  }

  // Handle web audio data
  private handleAudioDataWeb(event: Event): void {
    const target = event.target as BluetoothRemoteGATTCharacteristic;
    const value = target.value;
    if (!value) return;

    if (value.byteLength >= 12 && value.getUint8(0) === 0x41 && value.getUint8(1) === 0x44) {
      const seqNum = value.getUint16(2, true);
      const audioData = value.buffer.slice(12);
      this.audioCallbacks.forEach(cb => cb(audioData, seqNum));
    } else {
      this.audioCallbacks.forEach(cb => cb(value.buffer, this.receivedPackets++));
    }
  }

  // Handle native status
  private handleStatusUpdateNative(value: DataView): void {
    if (value.byteLength >= 20 && value.getUint8(0) === 0x41 && value.getUint8(1) === 0x53) {
      const status = this.parseStatus(value);
      this.statusCallbacks.forEach(cb => cb(status));
    }
  }

  // Handle web status
  private handleStatusUpdateWeb(event: Event): void {
    const target = event.target as BluetoothRemoteGATTCharacteristic;
    const value = target.value;
    if (!value || value.byteLength < 20) return;

    if (value.getUint8(0) === 0x41 && value.getUint8(1) === 0x53) {
      const status = this.parseStatus(value);
      this.statusCallbacks.forEach(cb => cb(status));
    }
  }

  private parseStatus(value: DataView): DeviceStatus {
    return {
      state: value.getUint8(2) as RecordingState,
      batteryPercent: value.getUint8(3),
      currentGain: value.getFloat32(4, true),
      sampleRate: value.getUint32(8, true),
      chunkDuration: value.getUint16(12, true),
      bytesRecorded: value.getUint32(14, true),
      uptime: value.getUint32(18, true),
      vadEnabled: value.byteLength > 22 ? value.getUint8(22) === 1 : undefined,
      vadThreshold: value.byteLength > 26 ? value.getFloat32(23, true) : undefined,
    };
  }

  private notifyConnectionChange(connected: boolean): void {
    this.connectionCallbacks.forEach(cb => cb(connected));
  }
}

export const bleService = new BLEService();
