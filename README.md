# ADAU1701-TCPi-ESP32

WiFi bridge between **SigmaStudio** and the **ADAU1701 DSP** using a $3 ESP32.

Replaces the ICP1 / ICP3 / ICP5 USB dongles entirely. Supports real-time parameter updates without pops or clicks thanks to a full hardware safeload implementation.

---

## Features

- Full **SigmaStudio TCPi protocol** over WiFi (TCP port 8086)
- **Hardware safeload** — glitch-free parameter changes, no muting required
- **Chunked I2C writes** — prevents silent data corruption on large blocks
- **EEPROM write** support — use *Actions → Write Latest Compilation to E2Prom* directly from SigmaStudio for autonomous selfboot
- **Web interface** for WiFi credentials and GPIO pin configuration
- **Captive AP mode** — if no WiFi is configured, the ESP32 creates its own hotspot for first-time setup
- BOOT button triggers DSP hardware reset

---

## Hardware

| Component | Details |
|-----------|---------|
| ESP32 | Any generic ESP32 board (tested on 30-pin and 38-pin) |
| DSP board | JAB4 Wondom or any ADAU1701 board with I2C exposed |
| Connection | 4 wires: SCL, SDA, 3.3V, GND |

### Default pin mapping

| Function | GPIO |
|----------|------|
| I2C SCL  | 17   |
| I2C SDA  | 16   |
| DSP RESET | 21  |
| DSP SELFBOOT | 19 |
| Status LED | 2  |

All pins are configurable from the web interface — no need to modify the code.

### Wiring diagram

```
ADAU1701 / JAB4          ESP32
─────────────────        ──────────────
SCL  ───────────────────  GPIO17 (SCL)
SDA  ───────────────────  GPIO16 (SDA)
RESET ──────────────────  GPIO21
SELFBOOT ───────────────  GPIO19
3.3V ───────────────────  3.3V
GND  ───────────────────  GND
```

> **Note:** The JAB4 Wondom exposes I2C on connector J4 (SDA, SCL, 3.3V, GND).
> RESET and SELFBOOT may need to be wired directly to the ADAU1701 pins
> depending on your board.

---

## Installation

1. Install **Arduino IDE** with **ESP32 board support**
   (Boards Manager → search `esp32` by Espressif)

2. Clone or download this repository

3. Open `ADAU1701_TCPi_ESP32.ino` in Arduino IDE

4. Select your board: `Tools → Board → ESP32 Dev Module`

5. Flash to your ESP32

---

## First-time setup

On first boot (or after factory reset), the ESP32 starts in **AP mode**:

1. Connect your phone or PC to the WiFi network **`ADAU1701-ESP32`**
   Password: `adau1701`

2. Open **`http://192.168.4.1`** in your browser

3. Go to **Configuration** → enter your WiFi SSID and password → Save

4. The ESP32 reboots and connects to your network

5. Check the serial monitor for the assigned IP address, or look it up in your router

---

## SigmaStudio setup

1. Open your project in SigmaStudio

2. In the **Hardware Configuration** tab:
   
   - Connection type: **TCP/IP**
   - IP address: your ESP32's IP (e.g. `192.168.1.100`)
   - Port: `8086`

3. Click **Connect** — the LED on the ESP32 will turn on

4. Click **Link Compile Download** as usual

### Real-time parameter updates

Adjusting any parameter (EQ, volume, crossover...) while audio is playing
is **completely silent** — no pops, no clicks, no muting.

This is achieved using the ADAU1701 hardware safeload mechanism:
each parameter update is transferred atomically at the start of an audio frame,
so the DSP always sees complete and consistent coefficients.

### Writing to EEPROM (selfboot)

To make the DSP start autonomously without a computer:

1. Download your program normally
2. In SigmaStudio: **Actions → Write Latest Compilation to E2Prom**
3. Enable the SELFBOOT pin on your DSP board (jumper or solder bridge)
4. The ADAU1701 will now boot from EEPROM on power-up

---

## Web interface

| URL | Description |
|-----|-------------|
| `http://IP/` | Status page, DSP reset button |
| `http://IP/config` | WiFi and GPIO pin configuration |
| `http://IP/status` | JSON status (DSP state, IP, pins) |
| `http://IP/factory_reset` | Clear all settings, reboot in AP mode |

---

## Why this works better than a simple I2C bridge

Most ESP32 SigmaStudio implementations just forward I2C writes as-is.
This causes **audible pops and clicks** whenever a parameter is changed
while audio is playing, because the DSP reads partially-updated filter coefficients.

This firmware uses the **ADAU1701 hardware safeload** feature:

- For real-time parameter updates (`safeload=1` in the TCPi protocol),
  data is written to the safeload staging registers (0x0810–0x0819)
- A single **IST (Initiate Safeload Transfer)** bit triggers an atomic
  transfer at the start of the next audio frame
- Blocks larger than 5 words are split into multiple 5-word IST chunks
- The DSP **never** processes a partially-updated biquad

Additionally, large I2C writes (>120 bytes) are split into 30-word chunks
to prevent silent data corruption that can occur on ESP32 even with
`Wire.setBufferSize(2048)`.

---

## ICP vs ESP32 comparison

| Feature | ICP1/ICP3/ICP5 | This project |
|---------|---------------|-------------|
| Connection | USB (cable required) | WiFi |
| Cost | $30–40 | ~$3 |
| Safeload | No | Yes |
| Pops on parameter change | Yes | No |
| Web configuration | No | Yes |
| EEPROM write | Yes | Yes |
| Open source | No | Yes |

---

## Troubleshooting

**SigmaStudio cannot connect**
- Check the ESP32 IP address (serial monitor or router DHCP table)
- Make sure port 8086 is not blocked by a firewall
- The ESP32 and the PC must be on the same network

**DSP not responding after Download**
- Press the BOOT button on the ESP32 to hardware-reset the DSP
- Or call `http://IP/reset_dsp` from your browser

**Sound changes completely after parameter update**
- This should not happen with this firmware
- If it does, check your I2C wiring — loose connections cause intermittent corruption

**AP mode after every reboot**
- WiFi credentials are not saved — go through the setup again
- If the problem persists, the ESP32 may not be reaching your router (check signal strength)

---

## License

MIT License — see [LICENSE](LICENSE)

---

## Acknowledgements

- ADAU1701 safeload mechanism documented in the
  [Analog Devices ADAU1701 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ADAU1701.pdf) (page 37)
- TCPi protocol reverse-engineered by analysing traffic between
  SigmaStudio and ICP hardware
- Tested with JAB4 Wondom DSP board

