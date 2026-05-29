#!/bin/bash

echo "==========================================="
echo "🛠️ BSides Badge Flasher (macOS Compatible)"
echo "==========================================="
echo ""

### Step 1: Check Python version
PYVER=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")' 2>/dev/null)
if [[ -z "$PYVER" ]]; then
  echo "❌ Python3 not found. Please install Python 3.10 or newer."
  exit 1
fi

if (( $(echo "$PYVER < 3.10" | bc -l) )); then
  echo "❌ Python $PYVER found. Python 3.10+ is required."
  exit 1
fi

echo "✅ Python $PYVER detected."

### Step 2: Create a virtual environment
if [[ ! -d .venv ]]; then
  echo "📦 Creating virtual environment..."
  python3 -m venv .venv || { echo "❌ Failed to create virtual environment."; exit 1; }
fi

source .venv/bin/activate

### Step 3: Install esptool in the venv
if ! python -m esptool version &>/dev/null; then
  echo "📦 Installing esptool into virtual environment..."
  pip install --upgrade pip >/dev/null
  pip install esptool || { echo "❌ Failed to install esptool."; exit 1; }
else
  echo "✅ esptool is already installed in venv."
fi

### Step 4: Prompt user
echo ""
echo "🔌 Please plug in the badge"
read -p "👉 Press Enter when ready..."

### Step 5: Detect port
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -n 1)
if [[ -z "$PORT" ]]; then
  echo "❌ Could not detect badge. Is it in bootloader mode?"
  deactivate
  exit 1
fi

echo "✅ Badge found at $PORT"
echo ""

### Step 6: Flash firmware
echo "🚀 Flashing badge firmware..."

python -m esptool --chip esp32c3 --port "$PORT" --baud 115200 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size detect \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0x10000 BSides_Badge_2025.bin

### Step 7: Cleanup
deactivate

echo ""
echo "🎉 Flash complete. You can now unplug and use the badge!"
