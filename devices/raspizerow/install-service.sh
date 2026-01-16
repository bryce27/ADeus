#!/bin/bash
# Install ADeus as a systemd service to run on boot

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_NAME="adeus"

echo "=== ADeus Service Installer ==="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root: sudo $0"
    exit 1
fi

# Check if main exists
if [ ! -f "$SCRIPT_DIR/main" ]; then
    echo "Error: $SCRIPT_DIR/main not found. Run ./compile.sh first."
    exit 1
fi

# Prompt for environment variables if not set
if [ -z "$SUPABASE_URL" ]; then
    read -p "Enter your SUPABASE_URL: " SUPABASE_URL
fi

if [ -z "$AUTH_TOKEN" ]; then
    read -p "Enter your AUTH_TOKEN: " AUTH_TOKEN
fi

# Get the user who will run the service
ADEUS_USER="${SUDO_USER:-pi}"

echo ""
echo "Installing service with:"
echo "  User: $ADEUS_USER"
echo "  Directory: $SCRIPT_DIR"
echo ""

# Create the systemd service file
cat > /etc/systemd/system/${SERVICE_NAME}.service << EOF
[Unit]
Description=ADeus Audio Recorder
After=network-online.target sound.target
Wants=network-online.target

[Service]
Type=simple
User=$ADEUS_USER
WorkingDirectory=$SCRIPT_DIR
Environment="SUPABASE_URL=$SUPABASE_URL"
Environment="AUTH_TOKEN=$AUTH_TOKEN"
ExecStart=$SCRIPT_DIR/main
Restart=on-failure
RestartSec=10

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=adeus

[Install]
WantedBy=multi-user.target
EOF

# Reload systemd
systemctl daemon-reload

# Enable the service to start on boot
systemctl enable ${SERVICE_NAME}

echo ""
echo "=== Installation Complete! ==="
echo ""
echo "Commands:"
echo "  sudo systemctl start adeus     # Start now"
echo "  sudo systemctl stop adeus      # Stop"
echo "  sudo systemctl restart adeus   # Restart"
echo "  sudo systemctl status adeus    # Check status"
echo "  journalctl -u adeus -f         # View live logs"
echo ""
echo "The service will start automatically on boot."
echo ""

read -p "Start the service now? [Y/n] " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]] || [[ -z $REPLY ]]; then
    systemctl start ${SERVICE_NAME}
    echo ""
    echo "Service started! Check status with: sudo systemctl status adeus"
fi
