// =====================================================================
//  TS-570D  RS-232 <-> WiFi bridge  (ESP32-S3-Zero, Arduino-ESP32)
//
//  Replaces the USB-RS232 adapter (which caused interference on the
//  radio) by a WiFi link. The ESP32 talks to the TS-570D COM port through
//  a MAX3232 level converter and offers the raw byte stream on a TCP
//  port. piHPSDR-IF (or socat + rigctld) connects to that port.
//
//      TS-570D DB9 <--> MAX3232 module <--> ESP32-S3-Zero UART1
//                                             |
//                                           WiFi  -->  TCP port 7373
//
//  Radio:  menu 35 = "576-1"  (57600 bps, 8N1)
//  Board:  Waveshare ESP32-S3-Zero
//          Arduino IDE: board "ESP32S3 Dev Module",
//          USB CDC On Boot: Enabled (for the debug log on USB),
//          Flash Size: 4MB.
//
//  Status LED (on-board WS2812, GPIO21):
//      red    = no WiFi
//      blue   = WiFi up, no client
//      green  = client connected
//      white flash = data passing
// =====================================================================

#include <WiFi.h>
#include <ESPmDNS.h>

// ---------------------------------------------------------------------
//  Configuration
// ---------------------------------------------------------------------
static const char *WIFI_SSID     = "your-ssid";
static const char *WIFI_PASSWORD = "your-password";
static const char *HOSTNAME      = "ts570";      // -> ts570.local (mDNS)

// Fixed IP (recommended, so the client always finds the bridge).
// Set USE_STATIC_IP to false to use DHCP.
static const bool      USE_STATIC_IP = false;
static const IPAddress STATIC_IP (192, 168, 1, 57);
static const IPAddress GATEWAY   (192, 168, 1, 1);
static const IPAddress SUBNET    (255, 255, 255, 0);
static const IPAddress DNS       (192, 168, 1, 1);

static const uint16_t TCP_PORT   = 7373;
static const uint32_t CAT_BAUD   = 57600;        // must match TS-570D menu 35

// Pins (ESP32-S3-Zero header GP numbers = GPIO numbers)
//
// TS-570D COM connector (manual, appendix I-70; pin names are from the
// computer's point of view):
//   pin 2 "RXD"  radio OUTPUT  data sent by the radio
//   pin 3 "TXD"  radio INPUT   data received by the radio
//   pin 5  GND
//   pin 7 "RTS"  radio INPUT   when low, the radio does NOT send data
//   pin 8 "CTS"  radio OUTPUT  radio ready to receive
//
static const int PIN_UART_TX = 5;    // -> MAX3232 T1IN, T1OUT -> radio pin 3 (radio input)
static const int PIN_UART_RX = 6;    // <- MAX3232 R1OUT, R1IN <- radio pin 2 (radio output)
static const int PIN_RTS     = 7;    // -> MAX3232 T2IN, T2OUT -> radio pin 7 (RTS), held active
                                     //    (or, with a 1-channel module: bridge radio pins 7-8)
static const int PIN_LED     = 21;   // on-board WS2812 RGB LED

// ---------------------------------------------------------------------

static WiFiServer server(TCP_PORT);
static WiFiClient client;

static uint8_t  buf[512];
static uint32_t lastActivity = 0;
static uint32_t lastWifiTry  = 0;

static void led(uint8_t r, uint8_t g, uint8_t b) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(PIN_LED, r, g, b);
#else
  neopixelWrite(PIN_LED, r, g, b);
#endif
}

// Steady colour for the current state; a short white flash on traffic.
static void showState() {
  if (millis() - lastActivity < 30) {
    led(20, 20, 20);
  } else if (WiFi.status() != WL_CONNECTED) {
    led(25, 0, 0);
  } else if (client && client.connected()) {
    led(0, 25, 0);
  } else {
    led(0, 0, 25);
  }
}

static void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  if (USE_STATIC_IP) {
    WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS);
  }
  WiFi.setSleep(false);              // no power save: lowest latency
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiTry = millis();
}

void setup() {
  Serial.begin(115200);              // debug log on USB-CDC
  led(25, 0, 0);

  // RTS held active: on the RS-232 side this is +V ("on"); the TS-570D
  // does not send anything while its RTS input (pin 7) is low.
  pinMode(PIN_RTS, OUTPUT);
  digitalWrite(PIN_RTS, LOW);        // TTL low -> MAX3232 output high (+V, active)

  Serial1.setRxBufferSize(1024);
  Serial1.begin(CAT_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);

  startWifi();
  Serial.printf("TS-570 bridge: connecting to %s ...\n", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi up, IP %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi not up yet, will keep trying");
  }

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("ts570cat", "tcp", TCP_PORT);
  }
  server.begin();
  server.setNoDelay(true);
  Serial.printf("TCP server on port %u, UART %lu 8N1\n", TCP_PORT, (unsigned long)CAT_BAUD);
}

void loop() {
  // --- WiFi watchdog -------------------------------------------------
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 10000) {
    Serial.println("WiFi lost, reconnecting");
    WiFi.disconnect();
    startWifi();
  }

  // --- New client: replaces the old one (the client always wins) -----
  WiFiClient incoming = server.available();
  if (incoming) {
    if (client && client.connected()) {
      Serial.println("new client replaces the old one");
      client.stop();
    }
    client = incoming;
    client.setNoDelay(true);
    while (Serial1.available()) { Serial1.read(); }   // drop stale radio bytes
    Serial.printf("client %s connected\n", client.remoteIP().toString().c_str());
  }

  // --- TCP -> radio ----------------------------------------------------
  if (client && client.connected()) {
    int n = client.available();
    if (n > 0) {
      if (n > (int)sizeof(buf)) { n = sizeof(buf); }
      n = client.read(buf, n);
      if (n > 0) {
        Serial1.write(buf, n);
        lastActivity = millis();
      }
    }
  } else if (client) {
    Serial.println("client gone");
    client.stop();
  }

  // --- radio -> TCP ----------------------------------------------------
  int m = Serial1.available();
  if (m > 0) {
    if (m > (int)sizeof(buf)) { m = sizeof(buf); }
    m = Serial1.readBytes(buf, m);
    if (client && client.connected()) {
      client.write(buf, m);
    }
    lastActivity = millis();
  }

  showState();
  delay(1);                          // yield to WiFi; ~1 ms latency
}
