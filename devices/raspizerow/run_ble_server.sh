#!/bin/bash
# Run the ADeus BLE GATT Server for Chrome Web Bluetooth

echo "ADeus BLE GATT Server"
echo "====================="
echo ""

# Check for required packages
if ! python3 -c "import dbus" 2>/dev/null; then
    echo "Installing required packages..."
    sudo apt-get update
    sudo apt-get install -y python3-dbus python3-gi
fi

# Make sure bluetooth is running
echo "Starting Bluetooth service..."
sudo systemctl start bluetooth
sleep 1

# Power on the adapter
sudo hciconfig hci0 up 2>/dev/null

# Run the GATT server
echo "Starting GATT server..."
echo ""
sudo python3 "$(dirname "$0")/ble_gatt_server.py"
