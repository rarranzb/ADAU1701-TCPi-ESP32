// =============================================================
// ADAU1701-TCPi-ESP32  v1.0
// WiFi bridge between SigmaStudio and ADAU1701 DSP
//
// Features:
//   - Full SigmaStudio TCPi protocol over WiFi (port 8086)
//   - Hardware safeload: glitch-free real-time parameter updates
//   - Chunked I2C writes: prevents silent data corruption
//   - EEPROM selfboot: save program from web interface
//   - Web portal: WiFi + GPIO pin configuration
//   - AP mode for first-time setup
//
// https://github.com/rarranzb/ADAU1701-TCPi-ESP32
// License: MIT
//   - Captures all sl=0 writes during Download
//   - http://IP/ -> button "Save to EEPROM"
//   - Writes captured data to EEPROM in ADAU1701 selfboot format
//   - Replaces "Actions -> Write Latest Compilation to E2Prom"
//     which does not work over TCP/IP in SigmaStudio
//
// https://github.com/rarranzb/ADAU1701-TCPi-ESP32
// License: MIT
// =============================================================

#include <WiFi.h>
#include <Wire.h>
#include <WebServer.h>
#include <Preferences.h>

// ── Factory defaults ──────────────────────────────────────────
#define FACTORY_SSID      ""
#define FACTORY_PASSWORD  ""

// ── AP mode ───────────────────────────────────────────────────
#define AP_SSID           "ADAU1701-ESP32"
#define AP_PASSWORD       "adau1701"

// ── Default pins ──────────────────────────────────────────────
#define DEFAULT_SCL       17
#define DEFAULT_SDA       16
#define DEFAULT_RESET     21
#define DEFAULT_SELFBOOT  19
#define DEFAULT_LED        2
#define BOOT_BUTTON_PIN    0

// ── DSP ───────────────────────────────────────────────────────
#define DSP_I2C_ADDR      0x34
#define EEPROM_I2C_ADDR   0x50
#define TCP_PORT          8086

// ── ADAU1701 memory map ───────────────────────────────────────
#define PARAM_RAM_END     0x03FF
#define PROG_RAM_START    0x0400
#define PROG_RAM_END      0x07FF
#define CTRL_REG_START    0x0800

// ── Safeload registers ────────────────────────────────────────
#define SAFELOAD_DATA_0   0x0810
#define SAFELOAD_ADDR_0   0x0815
#define IST_BIT           0x20

// ── TCPi protocol ─────────────────────────────────────────────
#define CTRL_WRITE        0x09
#define CTRL_READ_REQ     0x0A
#define CTRL_READ_RESP    0x0B
#define BUFFER_SIZE       (1024 * 16)

// ── EEPROM selfboot capture ───────────────────────────────────
// Selfboot format per record: [len(2)] [addr(2)] [data(len)]
// End marker: 0x00 0x00
// 24LC256 = 32KB, page size = 64 bytes
#define EEPROM_MAX_SIZE   (32 * 1024)
#define EEPROM_PAGE_SIZE  64
#define CAPTURE_MAX_SIZE  (28 * 1024)   // leave margin

uint8_t  captureBuffer[CAPTURE_MAX_SIZE];
int      captureLen    = 0;
bool     captureReady  = false;   // true after DSPRUN=1 seen

// ── Globals ───────────────────────────────────────────────────
Preferences  prefs;
WiFiServer*  tcpServer = nullptr;
WiFiClient   client;
WebServer    httpServer(80);

bool     apMode          = false;
bool     dspRunning      = false;
uint8_t  lastCoreCtrl[2] = {0x00, 0x1C};
uint8_t  rxBuffer[BUFFER_SIZE];
int      rxLen           = 0;

String   savedSSID, savedPassword;
int      pinSCL, pinSDA, pinRESET, pinSELFBOOT, pinLED;

// ── Prototypes ────────────────────────────────────────────────
void loadConfig();
void saveWiFi(const String& ssid, const String& pass);
void savePins(int scl, int sda, int rst, int sb, int led);
bool connectWiFi();
void startAP();
void setupHTTP();
void initHardware();
void blinkLED(int n);
void scanI2C();
void resetDSP();
void captureWrite(uint16_t address, uint8_t* data, uint16_t dataLen);
bool writeEEPROM();
bool eepromWritePage(uint16_t memAddr, uint8_t* data, int len);
int  processBuffer(uint8_t* buf, int len);
void handleWrite(uint8_t chipAddr, uint16_t address, uint8_t* data, uint16_t dataLen, uint8_t safeload);
void handleRead(uint8_t chipAddr, uint16_t address, uint16_t nBytes);
void safeloadChunk(uint16_t address, uint8_t* data, int words);
void directWrite(uint8_t i2cAddr, uint16_t regAddr, uint8_t* data, uint16_t dataLen);

// =============================================================
// SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] ADAU1701-TCPi-ESP32 v1.0");

  loadConfig();
  initHardware();
  scanI2C();

  if (!connectWiFi()) {
    Serial.println("[WiFi] Failed -> AP mode");
    startAP();
  }

  setupHTTP();
  if (!apMode) {
    tcpServer = new WiFiServer(TCP_PORT);
    tcpServer->begin();
  }

  blinkLED(apMode ? 10 : 2);
  if (apMode)
    Serial.printf("[AP] SSID:'%s' pass:'%s' -> http://192.168.4.1\n", AP_SSID, AP_PASSWORD);
  else
    Serial.printf("[BOOT] Ready  TCP:%d  HTTP:80\n", TCP_PORT);
}

// =============================================================
// LOOP
// =============================================================
void loop() {
  httpServer.handleClient();
  if (apMode) return;

  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      Serial.println("[BTN] DSP reset");
      blinkLED(3);
      resetDSP();
      while (digitalRead(BOOT_BUTTON_PIN) == LOW) delay(10);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost -> reconnecting...");
    if (!connectWiFi()) { startAP(); setupHTTP(); return; }
  }

  if (!client || !client.connected()) {
    digitalWrite(pinLED, LOW);
    client = tcpServer->available();
    if (client) {
      Serial.printf("[TCP] Connection from %s\n", client.remoteIP().toString().c_str());
      digitalWrite(pinLED, HIGH);
      blinkLED(2);
      // Reset capture on new connection (new Download incoming)
      captureLen   = 0;
      captureReady = false;
      rxLen        = 0;
      dspRunning   = false;
    }
  }

  if (client && client.connected() && client.available()) {
    int n = client.readBytes(rxBuffer + rxLen, BUFFER_SIZE - rxLen);
    rxLen += n;
    int consumed = processBuffer(rxBuffer, rxLen);
    if (consumed > 0 && consumed < rxLen)
      memmove(rxBuffer, rxBuffer + consumed, rxLen - consumed);
    rxLen = (consumed <= rxLen) ? rxLen - consumed : 0;
  }
}

// =============================================================
// NVS
// =============================================================
void loadConfig() {
  prefs.begin("tcpi", true);
  savedSSID    = prefs.getString("ssid",      FACTORY_SSID);
  savedPassword= prefs.getString("password",  FACTORY_PASSWORD);
  pinSCL       = prefs.getInt("pin_scl",      DEFAULT_SCL);
  pinSDA       = prefs.getInt("pin_sda",      DEFAULT_SDA);
  pinRESET     = prefs.getInt("pin_reset",    DEFAULT_RESET);
  pinSELFBOOT  = prefs.getInt("pin_selfboot", DEFAULT_SELFBOOT);
  pinLED       = prefs.getInt("pin_led",      DEFAULT_LED);
  prefs.end();
  Serial.printf("[NVS] SSID:%s  SCL:%d SDA:%d RST:%d SB:%d LED:%d\n",
    savedSSID.c_str(), pinSCL, pinSDA, pinRESET, pinSELFBOOT, pinLED);
}

void saveWiFi(const String& ssid, const String& pass) {
  prefs.begin("tcpi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("password", pass);
  prefs.end();
}

void savePins(int scl, int sda, int rst, int sb, int led) {
  prefs.begin("tcpi", false);
  prefs.putInt("pin_scl",      scl);
  prefs.putInt("pin_sda",      sda);
  prefs.putInt("pin_reset",    rst);
  prefs.putInt("pin_selfboot", sb);
  prefs.putInt("pin_led",      led);
  prefs.end();
}

// =============================================================
// HARDWARE
// =============================================================
void initHardware() {
  pinMode(pinLED,          OUTPUT); digitalWrite(pinLED,      LOW);
  pinMode(pinSELFBOOT,     OUTPUT); digitalWrite(pinSELFBOOT, LOW);
  pinMode(pinRESET,        OUTPUT); digitalWrite(pinRESET,    HIGH);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  Wire.setBufferSize(2048);
  Wire.begin(pinSDA, pinSCL, 400000);
  Wire.setTimeOut(50);
  Serial.printf("[I2C] SCL=%d SDA=%d 400kHz\n", pinSCL, pinSDA);
}

// =============================================================
// WIFI
// =============================================================
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
  Serial.printf("[WiFi] Connecting to '%s'", savedSSID.c_str());
  for (int t = 0; t < 20 && WiFi.status() != WL_CONNECTED; t++) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
    apMode = false; return true;
  }
  Serial.println("\n[WiFi] Failed");
  return false;
}

void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  apMode = true;
}

// =============================================================
// EEPROM CAPTURE
// During sl=0 (Download), capture every DSP write in selfboot
// format: [len(2BE)] [addr(2BE)] [data...]
// =============================================================
void captureWrite(uint16_t address, uint8_t* data, uint16_t dataLen) {
  // ADAU1701 selfboot I2C master has limited buffer.
  // Split large writes into chunks of max 32 words to be safe.
  // Word sizes: Param RAM = 4 bytes, Prog RAM = 5 bytes, Ctrl = 2 bytes
  int wordSize = 4;
  if (address >= PROG_RAM_START && address <= PROG_RAM_END) wordSize = 5;
  else if (address >= CTRL_REG_START) wordSize = 2;

  int maxWords  = 32;
  int chunkSize = maxWords * wordSize;
  int offset    = 0;

  while (offset < dataLen) {
    int      bytes   = min(dataLen - offset, chunkSize);
    uint16_t addr    = address + (offset / wordSize);
    uint8_t* chunk   = data + offset;

    int needed = 6 + bytes;  // 1 opcode + 2 count + 1 chipAddr + 2 regAddr + bytes
    if (captureLen + needed + 2 > CAPTURE_MAX_SIZE) {
      Serial.println("[CAP] Buffer full, skipping");
      return;
    }

    // ADAU1701 selfboot EEPROM format (datasheet Table 19):
    // 0x01               = Write message type
    // [len_hi][len_lo]   = chipAddr(1) + regAddr(2) + dataBytes
    // 0x00               = chip address (always 0x00 in EEPROM messages)
    // [addr_hi][addr_lo] = register address
    // [data...]
    uint16_t count = bytes + 3;  // chipAddr(1) + regAddr(2) + data
    captureBuffer[captureLen++] = 0x01;                  // Write opcode
    captureBuffer[captureLen++] = (count >> 8) & 0xFF;
    captureBuffer[captureLen++] = count & 0xFF;
    captureBuffer[captureLen++] = 0x00;                  // chip address
    captureBuffer[captureLen++] = (addr >> 8) & 0xFF;
    captureBuffer[captureLen++] = addr & 0xFF;
    memcpy(captureBuffer + captureLen, chunk, bytes);
    captureLen += bytes;

    offset += bytes;
  }
}

// =============================================================
// EEPROM WRITE - selfboot format
// 24LC256: page size 64 bytes, write cycle ~5ms
// =============================================================
bool writeEEPROM() {
  if (captureLen == 0) {
    Serial.println("[EEPROM] Nothing captured");
    return false;
  }

  // Append end marker 0x0000
  captureBuffer[captureLen++] = 0x00;
  captureBuffer[captureLen++] = 0x00;

  Serial.printf("[EEPROM] Writing %d bytes to 0x%02X...\n", captureLen, EEPROM_I2C_ADDR);

  int offset = 0;
  while (offset < captureLen) {
    // Calculate bytes remaining in current page
    int pageOffset = (offset) % EEPROM_PAGE_SIZE;
    int bytes = min(captureLen - offset, EEPROM_PAGE_SIZE - pageOffset);

    if (!eepromWritePage(offset, captureBuffer + offset, bytes)) {
      Serial.printf("[EEPROM] Write failed at offset %d\n", offset);
      return false;
    }
    offset += bytes;

    // Progress log every 1KB
    if (offset % 1024 == 0)
      Serial.printf("[EEPROM] %d / %d bytes\n", offset, captureLen);
  }

  Serial.println("[EEPROM] Done!");
  return true;
}

bool eepromWritePage(uint16_t memAddr, uint8_t* data, int len) {
  Wire.beginTransmission(EEPROM_I2C_ADDR);
  Wire.write((memAddr >> 8) & 0xFF);
  Wire.write(memAddr & 0xFF);
  Wire.write(data, len);
  uint8_t err = Wire.endTransmission(true);
  if (err != 0) {
    Serial.printf("[EEPROM] I2C err=%d at 0x%04X\n", err, memAddr);
    return false;
  }
  // Wait for write cycle to complete (24LC256 max 5ms)
  delay(6);
  return true;
}

// =============================================================
// WEB INTERFACE
// =============================================================
String htmlHead(const String& title) {
  return R"(<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>)" + title + R"(</title>
<style>
  *{box-sizing:border-box}
  body{font-family:sans-serif;max-width:480px;margin:30px auto;padding:0 16px;color:#222}
  h2{margin-bottom:4px}
  h3{margin:24px 0 8px;border-bottom:1px solid #ddd;padding-bottom:4px}
  nav{margin-bottom:20px}
  nav a{margin-right:14px;color:#0078d4;text-decoration:none;font-size:14px}
  nav a:hover{text-decoration:underline}
  .card{background:#f7f7f7;border:1px solid #e0e0e0;border-radius:6px;
        padding:14px;margin-bottom:16px;font-size:14px}
  .card b{display:inline-block;width:110px;color:#555}
  label{display:block;font-size:13px;color:#555;margin-top:10px}
  input[type=text],input[type=password],input[type=number]{
    width:100%;padding:8px 10px;margin-top:4px;border:1px solid #ccc;
    border-radius:4px;font-size:15px}
  .row{display:flex;gap:12px}.row>div{flex:1}
  .btn{display:block;width:100%;padding:10px;margin-top:12px;border:none;
    border-radius:4px;font-size:15px;cursor:pointer;color:#fff}
  .btn-blue{background:#0078d4}.btn-blue:hover{background:#005fa3}
  .btn-green{background:#28a745}.btn-green:hover{background:#1e7e34}
  .btn-red{background:#dc3545}.btn-red:hover{background:#a71d2a}
  .btn-gray{background:#6c757d}.btn-gray:hover{background:#545b62}
  .ok{background:#d4edda;border:1px solid #c3e6cb;border-radius:4px;
    padding:10px;color:#155724;margin-top:12px}
  .warn{background:#fff3cd;border:1px solid #ffc107;border-radius:4px;
    padding:10px;color:#856404;margin-top:12px}
  .err{background:#f8d7da;border:1px solid #f5c6cb;border-radius:4px;
    padding:10px;color:#721c24;margin-top:12px}
  small{color:#888;font-size:12px}
</style></head><body>
<h2>ADAU1701-TCPi-ESP32</h2>
<nav><a href='/'>Status</a><a href='/config'>Configuration</a></nav>
)";
}

void setupHTTP() {
  httpServer.close();

  // ── Status page ────────────────────────────────────────────
  httpServer.on("/", []() {
    String ip   = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    String body = htmlHead("ADAU1701-TCPi-ESP32");

    body += "<div class='card'>";
    body += "<div><b>IP:</b>" + ip + "</div>";
    if (!apMode) {
      body += "<div><b>Network:</b>" + savedSSID + "</div>";
      body += "<div><b>DSP:</b>" + String(dspRunning ? "&#x25CF; Running" : "&#x25CB; Stopped") + "</div>";
      body += "<div><b>TCP port:</b>" + String(TCP_PORT) + "</div>";

      // EEPROM capture status
      if (captureReady) {
        body += "<div><b>Capture:</b>&#x2705; Ready (" + String(captureLen) + " bytes)</div>";
      } else if (captureLen > 0) {
        body += "<div><b>Capture:</b>&#x23F3; Downloading... (" + String(captureLen) + " bytes)</div>";
      } else {
        body += "<div><b>Capture:</b>&#x25CB; Waiting for Download</div>";
      }
    }
    body += "</div>";

    if (apMode) {
      body += "<div class='warn'>Connect to <b>" + String(AP_SSID) +
              "</b> and open <b>http://192.168.4.1/config</b> to set up WiFi.</div>";
    }

    // ── DSP Reset ──────────────────────────────────────────
    body += "<form action='/reset_dsp' method='POST'>"
            "<button class='btn btn-gray'>Reset DSP</button></form>";

    // ── Save to EEPROM ─────────────────────────────────────
    if (captureReady) {
      body += "<form action='/save_eeprom' method='POST'>"
              "<button class='btn btn-green'>&#x1F4BE; Save to EEPROM (selfboot)</button></form>"
              "<small style='display:block;margin-top:6px'>"
              "Writes the current program to EEPROM. "
              "Enable SELFBOOT on your DSP board to boot autonomously.</small>";
    } else {
      body += "<div class='warn' style='margin-top:12px'>"
              "&#x26A0; Do a <b>Link Compile Download</b> in SigmaStudio first, "
              "then the Save to EEPROM button will appear.</div>";
    }

    body += "</body></html>";
    httpServer.send(200, "text/html", body);
  });

  // ── Save to EEPROM ─────────────────────────────────────────
  httpServer.on("/save_eeprom", HTTP_POST, []() {
    if (!captureReady) {
      httpServer.send(400, "text/html",
        htmlHead("Error") +
        "<div class='err'>No capture available. Do a Download first.</div>"
        "<script>setTimeout(()=>location.href='/',2000)</script></body></html>");
      return;
    }

    // Send immediate response, then write (writing takes a few seconds)
    httpServer.send(200, "text/html",
      htmlHead("Saving...") +
      "<div class='ok'>&#x23F3; Writing to EEPROM, please wait (~5 seconds)...</div>"
      "<script>setTimeout(()=>location.href='/eeprom_result',8000)</script></body></html>");

    // Write EEPROM
    bool ok = writeEEPROM();
    prefs.begin("tcpi", false);
    prefs.putBool("eeprom_ok", ok);
    prefs.end();
  });

  // ── EEPROM result ───────────────────────────────────────────
  httpServer.on("/eeprom_result", []() {
    prefs.begin("tcpi", true);
    bool ok = prefs.getBool("eeprom_ok", false);
    prefs.end();
    String body = htmlHead("EEPROM Result");
    if (ok) {
      body += "<div class='ok'>&#x2705; EEPROM written successfully!<br><br>"
              "<b>Next steps:</b><br>"
              "1. Enable SELFBOOT on your DSP board (jumper or pin HIGH)<br>"
              "2. Power cycle the board<br>"
              "3. The ADAU1701 will boot autonomously from EEPROM</div>";
    } else {
      body += "<div class='err'>&#x274C; EEPROM write failed.<br>"
              "Check I2C wiring and that your board has an EEPROM at 0x50.</div>";
    }
    body += "<a href='/'>Back</a></body></html>";
    httpServer.send(200, "text/html", body);
  });

  // ── Config page ─────────────────────────────────────────────
  httpServer.on("/config", []() {
    String body = htmlHead("Configuration");
    body += "<h3>WiFi</h3>"
            "<form action='/save_wifi' method='POST'>"
            "<label>SSID</label>"
            "<input type='text' name='ssid' value='" + savedSSID + "' required>"
            "<label>Password</label>"
            "<input type='password' name='pass' placeholder='leave empty to keep current'>"
            "<button class='btn btn-blue'>Save WiFi &amp; reboot</button></form>";

    body += "<h3>GPIO Pins</h3>"
            "<small>Do not use GPIO 6-11 (reserved for flash).</small>"
            "<form action='/save_pins' method='POST'>"
            "<div class='row'>"
            "<div><label>SCL</label><input type='number' name='scl' value='" + String(pinSCL) + "' min='0' max='39'></div>"
            "<div><label>SDA</label><input type='number' name='sda' value='" + String(pinSDA) + "' min='0' max='39'></div>"
            "</div><div class='row'>"
            "<div><label>RESET</label><input type='number' name='rst' value='" + String(pinRESET) + "' min='0' max='39'></div>"
            "<div><label>SELFBOOT</label><input type='number' name='sb' value='" + String(pinSELFBOOT) + "' min='0' max='39'></div>"
            "</div>"
            "<label>LED</label><input type='number' name='led' value='" + String(pinLED) + "' min='0' max='39'>"
            "<button class='btn btn-blue'>Save pins &amp; reboot</button></form>";

    body += "<h3>Factory reset</h3>"
            "<form action='/factory_reset' method='POST'>"
            "<button class='btn btn-red'>Clear all settings</button></form>"
            "</body></html>";
    httpServer.send(200, "text/html", body);
  });

  httpServer.on("/save_wifi", HTTP_POST, []() {
    if (!httpServer.hasArg("ssid") || httpServer.arg("ssid").isEmpty()) {
      httpServer.send(400, "text/plain", "Missing SSID"); return;
    }
    String newSSID = httpServer.arg("ssid");
    String newPass = httpServer.arg("pass");
    if (newPass.isEmpty()) newPass = savedPassword;
    saveWiFi(newSSID, newPass);

    // Try to connect while keeping AP alive so browser stays connected
    // WIFI_AP_STA: AP stays up during connection attempt
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(newSSID.c_str(), newPass.c_str());
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries++ < 20) delay(500);

    String body = htmlHead("WiFi Saved");
    if (WiFi.status() == WL_CONNECTED) {
      String ip = WiFi.localIP().toString();
      body += "<div class='ok'>&#x2705; Connected!<br><br>"
              "<b>Network:</b> " + newSSID + "<br>"
              "<b>IP address:</b> <a href='http://" + ip + "'>" + ip + "</a><br><br>"
              "Save this address to access the device on your network.<br>"
              "Rebooting in 5 seconds...</div>"
              "<script>setTimeout(()=>location.href='http://" + ip + "/',5000)</script>";
    } else {
      body += "<div class='warn'>&#x26A0; Could not connect to <b>" + newSSID + "</b>.<br>"
              "Check the password and try again. Rebooting in AP mode...</div>"
              "<script>setTimeout(()=>location.href='/',4000)</script>";
    }
    body += "</body></html>";
    httpServer.send(200, "text/html", body);
    delay(1500); ESP.restart();
  });

  httpServer.on("/save_pins", HTTP_POST, []() {
    int scl=httpServer.arg("scl").toInt(), sda=httpServer.arg("sda").toInt();
    int rst=httpServer.arg("rst").toInt(), sb=httpServer.arg("sb").toInt();
    int led=httpServer.arg("led").toInt();
    bool ok = scl>0 && sda>0 && rst>=0 && sb>=0 && led>=0;
    ok = ok && scl!=sda && scl!=rst && scl!=sb && scl!=led;
    ok = ok && sda!=rst && sda!=sb  && sda!=led;
    ok = ok && rst!=sb  && rst!=led && sb!=led;
    for (int p:{scl,sda,rst,sb,led}) if(p>=6 && p<=11) ok=false;
    if (!ok) {
      httpServer.send(400, "text/html",
        htmlHead("Error")+"<div class='err'>Invalid or duplicate pins. "
        "GPIO 6-11 reserved. <a href='/config'>Back</a></div></body></html>");
      return;
    }
    savePins(scl,sda,rst,sb,led);
    httpServer.send(200, "text/html",
      htmlHead("Saved")+"<div class='ok'>&#x2705; Saved. Rebooting...</div>"
      "<script>setTimeout(()=>location.href='/',3000)</script></body></html>");
    delay(1000); ESP.restart();
  });

  httpServer.on("/reset_dsp", HTTP_POST, []() {
    resetDSP();
    httpServer.send(200, "text/html",
      htmlHead("DSP Reset")+"<div class='ok'>&#x2705; DSP reset OK.</div>"
      "<script>setTimeout(()=>location.href='/',2000)</script></body></html>");
  });

  httpServer.on("/factory_reset", HTTP_POST, []() {
    prefs.begin("tcpi", false); prefs.clear(); prefs.end();
    httpServer.send(200, "text/html",
      htmlHead("Reset")+"<div class='ok'>Cleared. Rebooting in AP mode...</div></body></html>");
    delay(1000); ESP.restart();
  });

  httpServer.on("/status", []() {
    String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    httpServer.send(200, "application/json",
      "{\"dspRunning\":"  + String(dspRunning?"true":"false") +
      ",\"apMode\":"      + String(apMode?"true":"false") +
      ",\"captureReady\":"+ String(captureReady?"true":"false") +
      ",\"captureLen\":"  + String(captureLen) +
      ",\"ip\":\""        + ip + "\"}");
  });

  httpServer.begin();
}

// =============================================================
// TCPi PROTOCOL PARSER
// =============================================================
int processBuffer(uint8_t* buf, int len) {
  int pos = 0;
  while (pos < len) {
    uint8_t ctrl = buf[pos];
    if (ctrl == CTRL_WRITE) {
      if (pos + 10 > len) break;
      uint8_t  safeload = buf[pos + 1];
      uint16_t totalLen = (buf[pos + 3] << 8) | buf[pos + 4];
      uint8_t  chipAddr = buf[pos + 5];
      uint16_t dataLen  = (buf[pos + 6] << 8) | buf[pos + 7];
      uint16_t address  = (buf[pos + 8] << 8) | buf[pos + 9];
      if (pos + totalLen > len) { Serial.printf("[TCP] Frag %d/%d\n", len-pos, totalLen); break; }
      uint8_t* payload  = buf + pos + 10;
      Serial.printf("[W] 0x%04X len=%d sl=%d |", address, dataLen, safeload);
      for (int d = 0; d < min((int)dataLen, 8); d++) Serial.printf(" %02X", payload[d]);
      Serial.println();
      handleWrite(chipAddr, address, payload, dataLen, safeload);
      pos += totalLen;
    } else if (ctrl == CTRL_READ_REQ) {
      if (pos + 8 > len) break;
      uint16_t totalLen = (buf[pos+1]<<8)|buf[pos+2];
      uint8_t  chipAddr = buf[pos+3];
      uint16_t dataLen  = (buf[pos+4]<<8)|buf[pos+5];
      uint16_t address  = (buf[pos+6]<<8)|buf[pos+7];
      if (pos + totalLen > len) break;
      handleRead(chipAddr, address, dataLen);
      pos += totalLen;
    } else { pos++; }
  }
  return pos;
}

// =============================================================
// WRITE HANDLER
// =============================================================
void handleWrite(uint8_t chipAddr, uint16_t address, uint8_t* data, uint16_t dataLen, uint8_t safeload) {
  bool isDSP      = (chipAddr == 0x01 || chipAddr == DSP_I2C_ADDR);
  bool isParamRAM = (address <= PARAM_RAM_END);

  uint8_t i2cAddr = (chipAddr == 0x01) ? DSP_I2C_ADDR :
                    (chipAddr == 0x02) ? EEPROM_I2C_ADDR : chipAddr;

  // Capture sl=0 DSP writes BEFORE updating captureReady
  // This ensures the final 0x081C DSPRUN=1 write is included in the capture
  if (isDSP && safeload == 0 && !captureReady) {
    captureWrite(address, data, dataLen);
  }

  // Track DSP state and detect end of Download
  if (isDSP && address == 0x081C && dataLen >= 2) {
    lastCoreCtrl[0] = data[dataLen - 2];
    lastCoreCtrl[1] = data[dataLen - 1];
    bool wasRunning = dspRunning;
    dspRunning = (lastCoreCtrl[1] & 0x04) != 0;
    if (!wasRunning && dspRunning) {
      // DSPRUN just went high = Download complete
      captureReady = true;
      Serial.printf("[DSP] Running! Capture ready: %d bytes\n", captureLen);
    }
  }

  if (isDSP && isParamRAM && dspRunning && safeload == 1) {
    // Safeload: split into 5-word atomic IST chunks
    int totalWords = dataLen / 4, offset = 0, nChunks = 0;
    while (offset < totalWords) {
      int words = min(totalWords - offset, 5);
      safeloadChunk(address + offset, data + offset * 4, words);
      offset += words; nChunks++;
    }
    Serial.printf("[SAFELOAD] 0x%04X %d words %d ISTs\n", address, totalWords, nChunks);
  } else {
    directWrite(i2cAddr, address, data, dataLen);
  }
}

// =============================================================
// SAFELOAD
// =============================================================
void safeloadChunk(uint16_t address, uint8_t* data, int words) {
  for (int i = 0; i < words; i++) {
    uint16_t reg = SAFELOAD_DATA_0 + i;
    uint8_t* w   = data + (i * 4);
    Wire.beginTransmission(DSP_I2C_ADDR);
    Wire.write((reg>>8)&0xFF); Wire.write(reg&0xFF);
    Wire.write(0x00);
    Wire.write(w[0]); Wire.write(w[1]); Wire.write(w[2]); Wire.write(w[3]);
    Wire.endTransmission(true);
  }
  for (int i = 0; i < words; i++) {
    uint16_t reg  = SAFELOAD_ADDR_0 + i;
    uint16_t dest = address + i;
    Wire.beginTransmission(DSP_I2C_ADDR);
    Wire.write((reg>>8)&0xFF); Wire.write(reg&0xFF);
    Wire.write((dest>>8)&0xFF); Wire.write(dest&0xFF);
    Wire.endTransmission(true);
  }
  Wire.beginTransmission(DSP_I2C_ADDR);
  Wire.write(0x08); Wire.write(0x1C);
  Wire.write(lastCoreCtrl[0]);
  Wire.write(lastCoreCtrl[1] | IST_BIT);
  Wire.endTransmission(true);
}

// =============================================================
// CHUNKED I2C WRITE
// =============================================================
void directWrite(uint8_t i2cAddr, uint16_t regAddr, uint8_t* data, uint16_t dataLen) {
  int wordSize = 4;
  if (regAddr >= PROG_RAM_START && regAddr <= PROG_RAM_END) wordSize = 5;
  else if (regAddr >= CTRL_REG_START) wordSize = 2;
  int chunkBytes = 30 * wordSize, offset = 0;
  while (offset < dataLen) {
    int      bytes = min(dataLen - offset, chunkBytes);
    uint16_t addr  = regAddr + (offset / wordSize);
    Wire.beginTransmission(i2cAddr);
    Wire.write((addr>>8)&0xFF); Wire.write(addr&0xFF);
    Wire.write(data + offset, bytes);
    uint8_t err = Wire.endTransmission(true);
    if (err != 0) {
      Serial.printf("[I2C] ERR 0x%04X len=%d err=%d\n", addr, bytes, err);
      Wire.end(); delay(5);
      Wire.setBufferSize(2048);
      Wire.begin(pinSDA, pinSCL, 400000);
      Wire.setTimeOut(50);
    }
    offset += bytes;
  }
}

// =============================================================
// I2C READ
// =============================================================
void handleRead(uint8_t chipAddr, uint16_t address, uint16_t nBytes) {
  uint8_t i2cAddr = (chipAddr==0x01) ? DSP_I2C_ADDR :
                    (chipAddr==0x02) ? EEPROM_I2C_ADDR : chipAddr;
  Wire.beginTransmission(i2cAddr);
  Wire.write((address>>8)&0xFF); Wire.write(address&0xFF);
  Wire.endTransmission(false);
  Wire.requestFrom((int)i2cAddr, (int)nBytes, true);
  uint8_t rd[256]={0}; int idx=0;
  while (Wire.available() && idx<(int)nBytes && idx<256) rd[idx++]=Wire.read();
  uint8_t resp[270]; int sz=9+idx;
  resp[0]=CTRL_READ_RESP;
  resp[1]=(sz>>8)&0xFF; resp[2]=sz&0xFF;
  resp[3]=chipAddr;
  resp[4]=(idx>>8)&0xFF; resp[5]=idx&0xFF;
  resp[6]=(address>>8)&0xFF; resp[7]=address&0xFF;
  resp[8]=0x01;
  memcpy(resp+9,rd,idx);
  if (client && client.connected()) client.write(resp, 9+idx);
}

// =============================================================
void resetDSP() {
  dspRunning = false;
  digitalWrite(pinRESET, LOW); delay(10);
  digitalWrite(pinRESET, HIGH); delay(50);
  Serial.println("[DSP] Reset OK");
}

void scanI2C() {
  Serial.println("[I2C] Scanning:");
  for (uint8_t a=1; a<127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission()==0) Serial.printf("  Found 0x%02X\n", a);
  }
}

void blinkLED(int n) {
  bool wasOn = (digitalRead(pinLED)==HIGH);
  for (int i=0; i<n; i++) {
    digitalWrite(pinLED,HIGH); delay(100);
    digitalWrite(pinLED,LOW);  delay(100);
  }
  if (wasOn || (!apMode && client && client.connected())) digitalWrite(pinLED,HIGH);
}
