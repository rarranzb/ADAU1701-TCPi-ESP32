#!/usr/bin/env bash
# =============================================================
# ADAU1701-TCPi-ESP32 - One-line installer
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/rarranzb/ADAU1701-TCPi-ESP32/main/install.sh | bash
# =============================================================

set -e

BIN_URL="https://raw.githubusercontent.com/rarranzb/ADAU1701-TCPi-ESP32/main/firmware/ADAU1701_TCPi_ESP32.bin"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()    { echo -e "${GREEN}[✓]${NC} $1"; }
warning() { echo -e "${YELLOW}[!]${NC} $1"; }
error()   { echo -e "${RED}[✗]${NC} $1"; exit 1; }

echo ""
echo "  ADAU1701-TCPi-ESP32 Installer"
echo "  ================================"
echo ""

# ── Python ────────────────────────────────────────────────────
if ! command -v python3 &>/dev/null; then
  error "Python 3 not found. Install it from https://python.org"
fi
info "Python3: $(python3 --version)"

# ── esptool ───────────────────────────────────────────────────
info "Installing esptool..."
python3 -m pip install --quiet --upgrade esptool
info "esptool: $(python3 -m esptool version 2>&1 | head -1)"

# ── Detect port ───────────────────────────────────────────────
echo ""
warning "Make sure your ESP32 is connected via USB"
echo ""

detect_port() {
  if [[ "$OSTYPE" == "darwin"* ]]; then
    ls /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial* 2>/dev/null | head -1
  else
    ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -1
  fi
}

PORT=$(detect_port)
if [ -z "$PORT" ]; then
  warning "Port not detected automatically."
  echo -n "  Enter port (e.g. /dev/ttyUSB0 or COM3): "
  read -r PORT
fi
[ -z "$PORT" ] && error "No port specified."
info "Port: $PORT"

# ── Download ──────────────────────────────────────────────────
echo ""
info "Downloading firmware..."
curl -L --progress-bar "$BIN_URL" -o "$TMPDIR/firmware.bin"
info "Download complete ($(du -h $TMPDIR/firmware.bin | cut -f1))"

# ── Flash ─────────────────────────────────────────────────────
echo ""
info "Flashing $PORT ..."
echo ""

python3 -m esptool \
  --chip esp32 \
  --port "$PORT" \
  --baud 460800 \
  --before default_reset \
  --after hard_reset \
  write_flash \
    --flash_mode dio \
    --flash_freq 40m \
    --flash_size detect \
    0x0 "$TMPDIR/firmware.bin"

echo ""
info "Done!"
echo ""
echo "  Next steps:"
echo "  1. Connect to WiFi:  ADAU1701-ESP32  /  password: adau1701"
echo "  2. Open:             http://192.168.4.1"
echo "  3. Enter your WiFi credentials and save"
echo "  4. SigmaStudio:      USBi -> TCP/IP -> ESP32's IP : 8086"
echo ""