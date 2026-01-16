import { useSupabaseConfig } from '@/utils/useSupabaseConfig';
import {
  bleService,
  DeviceStatus,
  RecordingState,
} from '@/utils/bleService';
import { useCallback, useEffect, useState } from 'react';
import { toast } from 'sonner';
import { Button } from './ui/button';

interface DeviceControlProps {
  supabaseUrl: string;
  supabaseToken: string;
}

// Slider component for settings
function Slider({ 
  label, 
  value, 
  min, 
  max, 
  step, 
  onChange,
  unit = ''
}: { 
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  onChange: (value: number) => void;
  unit?: string;
}) {
  return (
    <div className="space-y-1">
      <div className="flex justify-between text-sm">
        <span className="text-gray-600 dark:text-gray-400">{label}</span>
        <span className="font-medium">{value}{unit}</span>
      </div>
      <input
        type="range"
        min={min}
        max={max}
        step={step}
        value={value}
        onChange={(e) => onChange(parseFloat(e.target.value))}
        className="w-full h-2 bg-gray-200 rounded-lg appearance-none cursor-pointer dark:bg-gray-700"
      />
    </div>
  );
}

export default function DeviceControl({ supabaseUrl, supabaseToken }: DeviceControlProps) {
  const [isAvailable, setIsAvailable] = useState(false);
  const [isConnected, setIsConnected] = useState(false);
  const [isConnecting, setIsConnecting] = useState(false);
  const [deviceName, setDeviceName] = useState<string | null>(null);
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [audioChunksReceived, setAudioChunksReceived] = useState(0);
  const [isUploading, setIsUploading] = useState(false);
  const [showSettings, setShowSettings] = useState(false);
  
  // Local settings state
  const [gain, setGain] = useState(1.0);
  const [vadEnabled, setVadEnabled] = useState(true);
  const [vadThreshold, setVadThreshold] = useState(0.02);

  // Check BLE availability on mount
  useEffect(() => {
    setIsAvailable(bleService.isAvailable());
  }, []);

  // Subscribe to BLE events
  useEffect(() => {
    const unsubConnection = bleService.onConnectionChange((connected) => {
      setIsConnected(connected);
      if (!connected) {
        setDeviceName(null);
        setStatus(null);
      }
    });

    const unsubStatus = bleService.onStatusUpdate((newStatus) => {
      setStatus(newStatus);
    });

    const unsubAudio = bleService.onAudioData((data, seqNum) => {
      setAudioChunksReceived((prev) => prev + 1);
      // Send audio to Supabase
      uploadAudioChunk(data);
    });

    return () => {
      unsubConnection();
      unsubStatus();
      unsubAudio();
    };
  }, []);

  // Upload audio chunk to Supabase
  const uploadAudioChunk = useCallback(async (data: ArrayBuffer) => {
    if (isUploading) return;
    
    setIsUploading(true);
    try {
      const response = await fetch(`${supabaseUrl}/functions/v1/process-audio`, {
        method: 'POST',
        headers: {
          'Authorization': `Bearer ${supabaseToken}`,
          'Content-Type': 'audio/wav',
        },
        body: data,
      });

      if (!response.ok) {
        console.error('Failed to upload audio:', response.statusText);
      }
    } catch (error) {
      console.error('Upload error:', error);
    } finally {
      setIsUploading(false);
    }
  }, [supabaseUrl, supabaseToken, isUploading]);

  // Connect to device
  const handleConnect = async () => {
    setIsConnecting(true);
    try {
      const device = await bleService.scanForDevices();
      if (device) {
        setDeviceName(device.name);
        await bleService.connect();
        toast.success(`Connected to ${device.name}`);
        // Request initial status
        await bleService.requestStatus();
      }
    } catch (error) {
      console.error('Connection error:', error);
      toast.error('Failed to connect to device');
    } finally {
      setIsConnecting(false);
    }
  };

  // Disconnect from device
  const handleDisconnect = async () => {
    await bleService.disconnect();
    toast.info('Disconnected from device');
  };

  // Recording controls
  const handleStartRecording = async () => {
    try {
      await bleService.startRecording();
      toast.success('Recording started');
    } catch (error) {
      toast.error('Failed to start recording');
    }
  };

  const handleStopRecording = async () => {
    try {
      await bleService.stopRecording();
      toast.success('Recording stopped');
    } catch (error) {
      toast.error('Failed to stop recording');
    }
  };

  const handlePauseRecording = async () => {
    try {
      await bleService.pauseRecording();
      toast.success('Recording paused');
    } catch (error) {
      toast.error('Failed to pause recording');
    }
  };

  // Format uptime as human readable
  const formatUptime = (seconds: number): string => {
    const hours = Math.floor(seconds / 3600);
    const mins = Math.floor((seconds % 3600) / 60);
    const secs = seconds % 60;
    
    if (hours > 0) {
      return `${hours}h ${mins}m`;
    } else if (mins > 0) {
      return `${mins}m ${secs}s`;
    }
    return `${secs}s`;
  };

  // Format bytes as human readable
  const formatBytes = (bytes: number): string => {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  };

  // Get recording state text and color
  const getStateInfo = (state: RecordingState) => {
    switch (state) {
      case RecordingState.RECORDING:
        return { text: 'Recording', color: 'text-red-500', dot: 'bg-red-500 animate-pulse' };
      case RecordingState.PAUSED:
        return { text: 'Paused', color: 'text-yellow-500', dot: 'bg-yellow-500' };
      case RecordingState.STOPPED:
        return { text: 'Stopped', color: 'text-gray-500', dot: 'bg-gray-500' };
      case RecordingState.ERROR:
        return { text: 'Error', color: 'text-red-700', dot: 'bg-red-700' };
      default:
        return { text: 'Unknown', color: 'text-gray-400', dot: 'bg-gray-400' };
    }
  };

  if (!isAvailable) {
    return (
      <div className="rounded-lg border border-dashed border-gray-300 p-4 text-center text-sm text-gray-500">
        <p>Bluetooth is not available on this device.</p>
        <p className="mt-1 text-xs">Use Chrome on Android or a Bluetooth-enabled desktop.</p>
      </div>
    );
  }

  if (!isConnected) {
    return (
      <div className="rounded-lg border border-gray-200 bg-gray-50 p-4 dark:border-gray-700 dark:bg-gray-800">
        <h3 className="mb-3 font-semibold">Connect to ADeus Device</h3>
        <p className="mb-4 text-sm text-gray-600 dark:text-gray-400">
          Connect to your Raspberry Pi to stream audio via Bluetooth.
        </p>
        <Button
          onClick={handleConnect}
          disabled={isConnecting}
          className="w-full"
        >
          {isConnecting ? (
            <>
              <span className="mr-2 inline-block h-4 w-4 animate-spin rounded-full border-2 border-white border-t-transparent" />
              Scanning...
            </>
          ) : (
            <>
              <BluetoothIcon className="mr-2 h-4 w-4" />
              Connect Device
            </>
          )}
        </Button>
      </div>
    );
  }

  const stateInfo = status ? getStateInfo(status.state) : null;

  return (
    <div className="rounded-lg border border-gray-200 bg-white p-4 dark:border-gray-700 dark:bg-gray-900">
      {/* Header */}
      <div className="mb-4 flex items-center justify-between">
        <div className="flex items-center space-x-2">
          <div className="h-2 w-2 rounded-full bg-green-500" />
          <span className="font-medium">{deviceName}</span>
        </div>
        <Button variant="outline" size="sm" onClick={handleDisconnect}>
          Disconnect
        </Button>
      </div>

      {/* Status */}
      {status && stateInfo && (
        <div className="mb-4 grid grid-cols-2 gap-3 text-sm">
          <div className="rounded bg-gray-100 p-2 dark:bg-gray-800">
            <div className="text-gray-500">Status</div>
            <div className={`flex items-center ${stateInfo.color}`}>
              <span className={`mr-2 h-2 w-2 rounded-full ${stateInfo.dot}`} />
              {stateInfo.text}
            </div>
          </div>
          <div className="rounded bg-gray-100 p-2 dark:bg-gray-800">
            <div className="text-gray-500">Uptime</div>
            <div>{formatUptime(status.uptime)}</div>
          </div>
          <div className="rounded bg-gray-100 p-2 dark:bg-gray-800">
            <div className="text-gray-500">Recorded</div>
            <div>{formatBytes(status.bytesRecorded)}</div>
          </div>
          <div className="rounded bg-gray-100 p-2 dark:bg-gray-800">
            <div className="text-gray-500">Gain</div>
            <div>{status.currentGain.toFixed(1)}x</div>
          </div>
          {status.batteryPercent < 255 && (
            <div className="rounded bg-gray-100 p-2 dark:bg-gray-800">
              <div className="text-gray-500">Battery</div>
              <div className={status.batteryPercent < 20 ? 'text-red-500' : ''}>
                {status.batteryPercent}%
              </div>
            </div>
          )}
          <div className="rounded bg-gray-100 p-2 dark:bg-gray-800">
            <div className="text-gray-500">Chunks</div>
            <div>{audioChunksReceived}</div>
          </div>
        </div>
      )}

      {/* Controls */}
      <div className="flex space-x-2">
        {status?.state === RecordingState.RECORDING ? (
          <>
            <Button
              variant="outline"
              className="flex-1"
              onClick={handlePauseRecording}
            >
              <PauseIcon className="mr-2 h-4 w-4" />
              Pause
            </Button>
            <Button
              variant="destructive"
              className="flex-1"
              onClick={handleStopRecording}
            >
              <StopIcon className="mr-2 h-4 w-4" />
              Stop
            </Button>
          </>
        ) : status?.state === RecordingState.PAUSED ? (
          <>
            <Button
              className="flex-1"
              onClick={handleStartRecording}
            >
              <PlayIcon className="mr-2 h-4 w-4" />
              Resume
            </Button>
            <Button
              variant="destructive"
              className="flex-1"
              onClick={handleStopRecording}
            >
              <StopIcon className="mr-2 h-4 w-4" />
              Stop
            </Button>
          </>
        ) : (
          <Button
            className="w-full"
            onClick={handleStartRecording}
          >
            <MicIcon className="mr-2 h-4 w-4" />
            Start Recording
          </Button>
        )}
      </div>

      {/* Upload indicator */}
      {isUploading && (
        <div className="mt-3 flex items-center justify-center text-sm text-gray-500">
          <span className="mr-2 inline-block h-3 w-3 animate-spin rounded-full border-2 border-gray-400 border-t-transparent" />
          Uploading audio...
        </div>
      )}

      {/* Settings toggle */}
      <button
        onClick={() => setShowSettings(!showSettings)}
        className="mt-3 flex w-full items-center justify-center text-sm text-gray-500 hover:text-gray-700 dark:hover:text-gray-300"
      >
        <SettingsIcon className="mr-1 h-4 w-4" />
        {showSettings ? 'Hide Settings' : 'Show Settings'}
      </button>

      {/* Settings panel */}
      {showSettings && (
        <div className="mt-3 space-y-4 rounded-lg bg-gray-50 p-3 dark:bg-gray-800">
          {/* Gain control */}
          <Slider
            label="Microphone Gain"
            value={gain}
            min={0.5}
            max={3.0}
            step={0.1}
            unit="x"
            onChange={async (val) => {
              setGain(val);
              try {
                await bleService.setGain(val);
              } catch (e) {
                console.error('Failed to set gain:', e);
              }
            }}
          />

          {/* VAD toggle */}
          <div className="flex items-center justify-between">
            <div>
              <div className="text-sm font-medium">Voice Activity Detection</div>
              <div className="text-xs text-gray-500">Only record when speech is detected</div>
            </div>
            <button
              onClick={async () => {
                const newValue = !vadEnabled;
                setVadEnabled(newValue);
                try {
                  await bleService.setVADEnabled(newValue);
                  toast.success(`VAD ${newValue ? 'enabled' : 'disabled'}`);
                } catch (e) {
                  console.error('Failed to set VAD:', e);
                }
              }}
              className={`relative inline-flex h-6 w-11 items-center rounded-full transition-colors ${
                vadEnabled ? 'bg-green-500' : 'bg-gray-300'
              }`}
            >
              <span
                className={`inline-block h-4 w-4 transform rounded-full bg-white transition-transform ${
                  vadEnabled ? 'translate-x-6' : 'translate-x-1'
                }`}
              />
            </button>
          </div>

          {/* VAD threshold */}
          {vadEnabled && (
            <Slider
              label="VAD Sensitivity"
              value={vadThreshold}
              min={0.005}
              max={0.1}
              step={0.005}
              onChange={async (val) => {
                setVadThreshold(val);
                try {
                  await bleService.setVADThreshold(val);
                } catch (e) {
                  console.error('Failed to set VAD threshold:', e);
                }
              }}
            />
          )}
        </div>
      )}
    </div>
  );
}

// Icons
function BluetoothIcon({ className }: { className?: string }) {
  return (
    <svg className={className} fill="none" viewBox="0 0 24 24" stroke="currentColor">
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M12 2l4 4-4 4m0 0l4 4-4 4m0-8V2m0 20v-8" />
    </svg>
  );
}

function MicIcon({ className }: { className?: string }) {
  return (
    <svg className={className} fill="none" viewBox="0 0 24 24" stroke="currentColor">
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M19 11a7 7 0 01-7 7m0 0a7 7 0 01-7-7m7 7v4m0 0H8m4 0h4m-4-8a3 3 0 01-3-3V5a3 3 0 116 0v6a3 3 0 01-3 3z" />
    </svg>
  );
}

function PlayIcon({ className }: { className?: string }) {
  return (
    <svg className={className} fill="none" viewBox="0 0 24 24" stroke="currentColor">
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M14.752 11.168l-3.197-2.132A1 1 0 0010 9.87v4.263a1 1 0 001.555.832l3.197-2.132a1 1 0 000-1.664z" />
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M21 12a9 9 0 11-18 0 9 9 0 0118 0z" />
    </svg>
  );
}

function PauseIcon({ className }: { className?: string }) {
  return (
    <svg className={className} fill="none" viewBox="0 0 24 24" stroke="currentColor">
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M10 9v6m4-6v6m7-3a9 9 0 11-18 0 9 9 0 0118 0z" />
    </svg>
  );
}

function StopIcon({ className }: { className?: string }) {
  return (
    <svg className={className} fill="none" viewBox="0 0 24 24" stroke="currentColor">
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M21 12a9 9 0 11-18 0 9 9 0 0118 0z" />
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M9 10a1 1 0 011-1h4a1 1 0 011 1v4a1 1 0 01-1 1h-4a1 1 0 01-1-1v-4z" />
    </svg>
  );
}

function SettingsIcon({ className }: { className?: string }) {
  return (
    <svg className={className} fill="none" viewBox="0 0 24 24" stroke="currentColor">
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.065 2.572c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.572 1.065c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.065-2.572c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z" />
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M15 12a3 3 0 11-6 0 3 3 0 016 0z" />
    </svg>
  );
}
