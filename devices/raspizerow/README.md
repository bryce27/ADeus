# ADeus Raspberry Pi Zero

Audio recorder for ADeus that captures audio from a USB microphone and sends it to your Supabase backend for transcription and storage.

## Features

- 🎤 **USB Microphone Support** - Works with any USB audio device
- 📡 **WiFi Mode** - Send audio directly to Supabase over HTTP
- 📱 **Bluetooth Mode** (optional) - Stream audio to phone app via BLE
- 💾 **Local Storage** - Optionally save audio files locally
- 🔧 **Configurable** - Adjust sample rate, gain, duration, and more

## Quick Start

### 1. Prerequisites

```bash
# Update system
sudo apt-get update

# Install required packages
sudo apt-get install -y cmake build-essential libasound2-dev libcurl4-openssl-dev
```

### 2. Build

```bash
cd ~/ADeus/devices/raspizerow
chmod +x compile.sh
./compile.sh
```

### 3. Set Environment Variables

```bash
export SUPABASE_URL="https://your-project.supabase.co"
export AUTH_TOKEN="your-supabase-anon-key"
```

Or add to `~/.bashrc` for persistence:
```bash
echo 'export SUPABASE_URL="https://your-project.supabase.co"' >> ~/.bashrc
echo 'export AUTH_TOKEN="your-supabase-anon-key"' >> ~/.bashrc
source ~/.bashrc
```

### 4. Run

```bash
./main
```

## Usage

```
./main [options]

Options:
  -h, --help              Print help
  -s, --save              Save audio to local file
  -g, --gain <float>      Microphone gain (volume multiplier)
  -d, --device <string>   ALSA audio device (default: plughw:1,0)
  -r, --rate <int>        Sample rate in Hz (default: 44100)
  -t, --duration <int>    Recording duration per chunk in seconds (default: 60)
  -b, --bluetooth         Use Bluetooth LE instead of WiFi (if compiled with BT)
  -v, --verbose           Enable verbose output
```

### Examples

```bash
# Default: USB mic, WiFi mode
./main

# Use built-in audio (if available)
./main -d plughw:0,0

# USB mic with 2x gain boost
./main -d plughw:1,0 -g 2.0

# Save audio locally while also uploading
./main --save

# Shorter recording chunks (30 seconds)
./main -t 30

# Bluetooth mode (requires BT compile)
./main --bluetooth
```

## Audio Device Configuration

### Finding Your Device

List available capture devices:
```bash
arecord -l
```

Example output:
```
**** List of CAPTURE Hardware Devices ****
card 0: bcm2835 [bcm2835], device 0: bcm2835 ALSA [bcm2835 ALSA]
card 1: Device [USB PnP Sound Device], device 0: USB Audio [USB Audio]
```

Device naming format: `plughw:CARD,DEVICE`
- Built-in: `plughw:0,0`
- USB mic: `plughw:1,0`

### Test Recording

```bash
# Record 3 seconds of audio
arecord -D plughw:1,0 -d 3 -f S32_LE -r 44100 test.wav

# Play it back (requires speaker)
aplay test.wav
```

## Bluetooth Setup (Optional)

### Option 1: Bluetooth Tethering (Easy)

Use your phone's internet connection via Bluetooth - no app changes needed!

```bash
# Run the setup script
sudo ./bluetooth/bt_tether.sh AA:BB:CC:DD:EE:FF

# Replace AA:BB:CC:DD:EE:FF with your phone's Bluetooth MAC address
# Find it by running: bluetoothctl scan on
```

Then enable "Bluetooth Tethering" on your phone. The Pi will use your phone's internet!

### Option 2: BLE Audio Streaming

Stream audio directly to the ADeus phone app via Bluetooth LE:

```bash
# Compile with Bluetooth support
./compile.sh --bluetooth

# Install Bluetooth packages
sudo apt-get install -y bluez libbluetooth-dev

# Run in Bluetooth mode
./main --bluetooth
```

## Troubleshooting

### "Unable to open PCM device"
- Check device name: `arecord -l`
- Make sure mic is plugged in
- Try: `sudo ./main` (permission issues)

### "SUPABASE_URL is not set"
- Set environment variables (see step 3 above)

### Audio quality issues
- Try adjusting gain: `./main -g 1.5`
- Check sample rate matches your mic

### Bluetooth not working
- Ensure Bluetooth is enabled: `sudo systemctl start bluetooth`
- Make device discoverable: `bluetoothctl discoverable on`

## Architecture

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│   USB Mic       │────▶│  ADeus Pi Zero  │────▶│    Supabase     │
│  (plughw:1,0)   │     │  (this program) │     │  (process-audio)│
└─────────────────┘     └─────────────────┘     └─────────────────┘
                               │                        │
                               │ WiFi/HTTP              │ Whisper
                               │ or BLE                 │ transcription
                               │                        │
                        ┌──────▼──────┐          ┌──────▼──────┐
                        │  Phone App  │          │  Database   │
                        │ (BLE mode)  │          │  (records)  │
                        └─────────────┘          └─────────────┘
```

## License

Part of the ADeus project - Open Source AI Wearable
