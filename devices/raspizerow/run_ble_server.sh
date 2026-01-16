#!/bin/bash
# Run the ADeus BLE GATT Server + Audio Recorder
# This single command handles everything:
#   - BLE GATT server for Chrome Web Bluetooth
#   - Audio recording via the C++ recorder
#   - Streaming audio to connected clients

SCRIPT_DIR="$(dirname "$0")"

# Check for required packages
if ! python3 -c "import dbus" 2>/dev/null; then
    echo "Installing required Python packages..."
    sudo apt-get update
    sudo apt-get install -y python3-dbus python3-gi
fi

# Check if recorder is compiled
if [ ! -f "$SCRIPT_DIR/build/main" ]; then
    echo "Recorder not compiled. Building now..."
    "$SCRIPT_DIR/compile.sh"
fi

# Run the integrated GATT server + recorder
sudo python3 "$SCRIPT_DIR/ble_gatt_server.py"
