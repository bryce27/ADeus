#!/bin/bash
# Bluetooth Tethering Setup for Raspberry Pi Zero
# This allows the Pi to use your phone's internet via Bluetooth PAN

set -e

PHONE_MAC="${1:-}"

print_usage() {
    echo "Usage: $0 <phone_bluetooth_mac_address>"
    echo ""
    echo "Example: $0 AA:BB:CC:DD:EE:FF"
    echo ""
    echo "To find your phone's MAC address:"
    echo "  1. Enable Bluetooth on your phone"
    echo "  2. Make it discoverable"
    echo "  3. Run: bluetoothctl scan on"
    echo "  4. Look for your phone's name and note the MAC address"
}

if [ -z "$PHONE_MAC" ]; then
    print_usage
    exit 1
fi

echo "=== ADeus Bluetooth Tethering Setup ==="
echo ""
echo "This will connect your Pi Zero to your phone via Bluetooth"
echo "to use its internet connection (no WiFi needed!)"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root: sudo $0 $PHONE_MAC"
    exit 1
fi

# Install required packages
echo "[1/5] Installing required packages..."
apt-get update -qq
apt-get install -y -qq bluez bluez-tools bridge-utils

# Enable Bluetooth service
echo "[2/5] Enabling Bluetooth service..."
systemctl enable bluetooth
systemctl start bluetooth

# Power on Bluetooth adapter
echo "[3/5] Configuring Bluetooth adapter..."
bluetoothctl power on
bluetoothctl agent on
bluetoothctl default-agent

# Pair with phone
echo "[4/5] Pairing with phone ($PHONE_MAC)..."
echo "Please make sure your phone is discoverable and accept the pairing request!"
bluetoothctl pair "$PHONE_MAC" || true
bluetoothctl trust "$PHONE_MAC"

# Connect to phone's network
echo "[5/5] Connecting to phone's network..."

# Create network connection script
cat > /usr/local/bin/adeus-bt-connect << 'EOF'
#!/bin/bash
PHONE_MAC="$1"

# Connect to phone
bluetoothctl connect "$PHONE_MAC"

# Wait for connection
sleep 2

# Set up network
bt-network -c "$PHONE_MAC" nap &

# Wait for interface
sleep 3

# Get IP via DHCP
dhclient bnep0 2>/dev/null || dhcpcd bnep0 2>/dev/null || true

# Verify connection
if ip addr show bnep0 2>/dev/null | grep -q "inet "; then
    echo "✓ Connected! IP address:"
    ip addr show bnep0 | grep "inet "
else
    echo "✗ Failed to get IP address. Make sure:"
    echo "  1. Bluetooth tethering is enabled on your phone"
    echo "  2. Your phone is paired with this Pi"
fi
EOF

chmod +x /usr/local/bin/adeus-bt-connect

# Create systemd service for auto-connect
cat > /etc/systemd/system/adeus-bluetooth.service << EOF
[Unit]
Description=ADeus Bluetooth Tethering
After=bluetooth.service
Wants=bluetooth.service

[Service]
Type=simple
ExecStart=/usr/local/bin/adeus-bt-connect $PHONE_MAC
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

# Enable the service
systemctl daemon-reload
systemctl enable adeus-bluetooth

echo ""
echo "=== Setup Complete! ==="
echo ""
echo "To connect now, run:"
echo "  sudo /usr/local/bin/adeus-bt-connect $PHONE_MAC"
echo ""
echo "Or reboot and it will auto-connect."
echo ""
echo "Make sure to enable 'Bluetooth Tethering' on your phone!"
