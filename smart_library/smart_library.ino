/*
 * Smart Library — ESP32 Firmware
 * ─────────────────────────────────────────────────────────
 * Hardware:
 *   - ESP32 DevKit
 *   - RC522 RFID module (SPI)
 *   - WS2812B LED strip
 *   - 4 push buttons (UP/DOWN/LEFT/RIGHT)
 *
 * LIBRARY CHANGE — remove ArduinoWebsockets, install instead:
 *   - WebSockets  by Markus Sattler  (search "WebSockets" in Library Manager,
 *                                     author = "Markus Sattler", version ≥ 2.4.0)
 *   - MFRC522     by GithubCommunity
 *   - FastLED     by Daniel Garcia
 *   - ArduinoJson by Benoit Blanchon
 *
 * Wiring (unchanged):
 *   RC522:  SDA=5, SCK=18, MOSI=23, MISO=19, RST=27, 3.3V, GND
 *   LEDs:   DATA=4, external 5V supply
 *   BTN UP=12  DOWN=13  LEFT=14  RIGHT=15  (INPUT_PULLUP, other leg → GND)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>      // ← Markus Sattler library
#include <ArduinoJson.h>
#include <MFRC522.h>
#include <FastLED.h>
#include <SPI.h>

// ─── Config ──────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "SHYAM";
const char* WIFI_PASSWORD = "0341AMIT";

// Railway URL — only the host, no protocol prefix
const char* WS_HOST = "smartlib-production.up.railway.app";
const uint16_t WS_PORT = 443;
const char* WS_PATH = "/?esp32=1";

// ─── Hardware pins ────────────────────────────────────────────────────────────
#define LED_PIN        4
#define NUM_LEDS       90
#define LED_BRIGHTNESS 80

#define RFID_SS_PIN    5
#define RFID_RST_PIN   27

#define BTN_UP         12
#define BTN_DOWN       13
#define BTN_LEFT       14
#define BTN_RIGHT      15

#define DEBOUNCE_MS    50
#define HOLD_MS        2000
#define RFID_COOLDOWN_MS 2000

// ─── Shelf layout (must match server DB) ─────────────────────────────────────
struct ShelfConfig {
  const char* name;
  uint8_t rows, cols, led_start;
};

const ShelfConfig SHELVES[] = {
  { "Shelf A", 3, 10, 0  },
  { "Shelf B", 3, 10, 30 },
  { "Shelf C", 3, 10, 60 },
};
const uint8_t NUM_SHELVES = 3;

// ─── Globals ─────────────────────────────────────────────────────────────────
CRGB leds[NUM_LEDS];
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
WebSocketsClient ws;

enum State { IDLE, NAVIGATING, CONFIRMING };
State state = IDLE;

int nav_shelf = 0, nav_row = 0, nav_col = 0, nav_book_id = -1;

bool          blink_on    = false;
unsigned long blink_last  = 0;
int           blink_led   = -1;
CRGB          blink_color = CRGB::Cyan;

unsigned long last_rfid_scan = 0;
bool ws_connected = false;

// ─── Button state ─────────────────────────────────────────────────────────────
struct Button {
  uint8_t pin;
  bool last_state;
  unsigned long press_time;
  bool hold_fired;
};

Button buttons[4] = {
  { BTN_UP,    HIGH, 0, false },
  { BTN_DOWN,  HIGH, 0, false },
  { BTN_LEFT,  HIGH, 0, false },
  { BTN_RIGHT, HIGH, 0, false },
};

// ─── Forward declarations ────────────────────────────────────────────────────
void startBlink(int led, CRGB color);
int  navToLED(int shelf, int row, int col);

// ─── WebSocket send ──────────────────────────────────────────────────────────
void sendJSON(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  ws.sendTXT(out);                   // ← Sattler API uses sendTXT()
  Serial.println("→ WS: " + out);
}

// ─── WebSocket event handler (Sattler callback style) ───────────────────────
void onWSEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_DISCONNECTED:
      ws_connected = false;
      Serial.println("[WS] Disconnected");
      break;

    case WStype_CONNECTED:
      ws_connected = true;
      Serial.printf("[WS] Connected to %s\n", (char*)payload);
      break;

    case WStype_TEXT: {
      String raw = String((char*)payload);
      Serial.println("← WS: " + raw);

      StaticJsonDocument<512> doc;
      if (deserializeJson(doc, raw) != DeserializationError::Ok) return;

      const char* cmd = doc["cmd"];
      if (!cmd) return;

      if (strcmp(cmd, "highlight_book") == 0) {
        int led = doc["led_index"];
        const char* color = doc["color"] | "green";
        startBlink(led, colorFromString(color));

      } else if (strcmp(cmd, "highlight_new") == 0) {
        startBlink((int)doc["led_index"], CRGB::Blue);

      } else if (strcmp(cmd, "start_relocation") == 0) {
        nav_book_id  = doc["book_id"];
        int cur_led  = doc["current_led"];
        ledToNavCoords(cur_led, nav_shelf, nav_row, nav_col);
        state = NAVIGATING;
        startBlink(cur_led, CRGB::Yellow);
        Serial.printf("[Nav] book_id=%d starting at led=%d\n", nav_book_id, cur_led);

      } else if (strcmp(cmd, "relocation_confirmed") == 0) {
        state = IDLE;
        flashConfirm((int)doc["led_index"]);

      } else if (strcmp(cmd, "slot_occupied") == 0) {
        flashError((int)doc["led_index"]);

      } else if (strcmp(cmd, "cancel") == 0) {
        state = IDLE;
        clearAll();
      }
      break;
    }

    case WStype_PING:
      Serial.println("[WS] Ping received");
      break;

    case WStype_PONG:
      Serial.println("[WS] Pong");
      break;

    default:
      break;
  }
}

// ─── LED helpers ─────────────────────────────────────────────────────────────
CRGB colorFromString(const char* s) {
  if (strcmp(s, "green")  == 0) return CRGB::Green;
  if (strcmp(s, "yellow") == 0) return CRGB::Yellow;
  if (strcmp(s, "blue")   == 0) return CRGB::Blue;
  if (strcmp(s, "red")    == 0) return CRGB::Red;
  if (strcmp(s, "cyan")   == 0) return CRGB::Cyan;
  return CRGB::White;
}

void startBlink(int led, CRGB color) {
  blink_led   = led;
  blink_color = color;
  blink_on    = true;
  blink_last  = millis();
}

void stopBlink() {
  if (blink_led >= 0 && blink_led < NUM_LEDS)
    leds[blink_led] = CRGB::Black;
  FastLED.show();
  blink_led = -1;
}

void updateBlink() {
  if (blink_led < 0) return;
  unsigned long now = millis();
  if (now - blink_last >= 500) {
    blink_on   = !blink_on;
    blink_last = now;
    leds[blink_led] = blink_on ? blink_color : CRGB::Black;
    FastLED.show();
  }
}

void clearAll() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  blink_led = -1;
}

void flashConfirm(int led) {
  stopBlink();
  for (int i = 0; i < 4; i++) {
    leds[led] = CRGB::Green; FastLED.show(); delay(120);
    leds[led] = CRGB::Black; FastLED.show(); delay(80);
  }
}

void flashError(int led) {
  for (int i = 0; i < 3; i++) {
    leds[led] = CRGB::Red;   FastLED.show(); delay(100);
    leds[led] = CRGB::Black; FastLED.show(); delay(100);
  }
  startBlink(navToLED(nav_shelf, nav_row, nav_col), CRGB::Cyan);
}

// ─── Coordinate helpers ───────────────────────────────────────────────────────
int navToLED(int shelf, int row, int col) {
  return SHELVES[shelf].led_start + row * SHELVES[shelf].cols + col;
}

void ledToNavCoords(int led, int& shelf, int& row, int& col) {
  for (int s = 0; s < NUM_SHELVES; s++) {
    int start = SHELVES[s].led_start;
    int end   = start + SHELVES[s].rows * SHELVES[s].cols - 1;
    if (led >= start && led <= end) {
      int offset = led - start;
      shelf = s;
      row   = offset / SHELVES[s].cols;
      col   = offset % SHELVES[s].cols;
      return;
    }
  }
  shelf = 0; row = 0; col = 0;
}

// ─── Navigation ───────────────────────────────────────────────────────────────
void moveNav(int drow, int dcol) {
  if (state != NAVIGATING) return;

  int new_row = nav_row + drow;
  int new_col = nav_col + dcol;
  int rows    = SHELVES[nav_shelf].rows;
  int cols    = SHELVES[nav_shelf].cols;

  // Column wrap → carry to next/prev row
  if (new_col < 0)       { new_row--; new_col = cols - 1; }
  else if (new_col >= cols) { new_row++; new_col = 0; }

  // Row clamp (stay on shelf)
  if (new_row < 0)     new_row = 0;
  if (new_row >= rows) new_row = rows - 1;

  nav_row = new_row;
  nav_col = new_col;

  int led = navToLED(nav_shelf, nav_row, nav_col);
  startBlink(led, CRGB::Cyan);
  Serial.printf("[Nav] shelf=%d row=%d col=%d led=%d\n", nav_shelf, nav_row, nav_col, led);
}

void confirmNav() {
  if (state != NAVIGATING) return;
  state = CONFIRMING;

  int led = navToLED(nav_shelf, nav_row, nav_col);

  // Fast white blink = hold detected
  for (int i = 0; i < 6; i++) {
    leds[led] = CRGB::White; FastLED.show(); delay(80);
    leds[led] = CRGB::Black; FastLED.show(); delay(60);
  }

  StaticJsonDocument<128> doc;
  doc["cmd"]       = "slot_selected";
  doc["book_id"]   = nav_book_id;
  doc["shelf_id"]  = nav_shelf + 1;   // server is 1-indexed
  doc["row"]       = nav_row;
  doc["col"]       = nav_col;
  doc["led_index"] = led;
  sendJSON(doc);
}

// ─── Button handling ──────────────────────────────────────────────────────────
void handleButtons() {
  for (int i = 0; i < 4; i++) {
    Button& btn     = buttons[i];
    bool    reading = digitalRead(btn.pin);

    if (reading == LOW && btn.last_state == HIGH) {
      btn.press_time = millis();
      btn.hold_fired = false;
    }

    if (reading == LOW && !btn.hold_fired) {
      if (millis() - btn.press_time >= HOLD_MS) {
        btn.hold_fired = true;
        confirmNav();
      }
    }

    if (reading == HIGH && btn.last_state == LOW) {
      if (!btn.hold_fired && millis() - btn.press_time >= DEBOUNCE_MS) {
        switch (btn.pin) {
          case BTN_UP:    moveNav(-1,  0); break;
          case BTN_DOWN:  moveNav( 1,  0); break;
          case BTN_LEFT:  moveNav( 0, -1); break;
          case BTN_RIGHT: moveNav( 0,  1); break;
        }
      }
    }

    btn.last_state = reading;
  }
}

// ─── RFID ─────────────────────────────────────────────────────────────────────
void handleRFID() {
  if (state != IDLE) return;
  if (millis() - last_rfid_scan < RFID_COOLDOWN_MS) return;
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (i > 0) uid += ":";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  Serial.println("[RFID] Scanned: " + uid);
  last_rfid_scan = millis();

  StaticJsonDocument<128> doc;
  doc["cmd"]      = "rfid_scan";
  doc["rfid_uid"] = uid;
  sendJSON(doc);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ─── WiFi ─────────────────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("[WiFi] Connecting");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[Boot] Smart Library ESP32");

  // Buttons
  pinMode(BTN_UP,    INPUT_PULLUP);
  pinMode(BTN_DOWN,  INPUT_PULLUP);
  pinMode(BTN_LEFT,  INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);

  // LEDs
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(LED_BRIGHTNESS);
  clearAll();

  // RFID
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("[RFID] Ready");

  // WiFi
  connectWiFi();

  // WebSocket — Sattler library, WSS with insecure TLS (no cert pinning)
  ws.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  ws.onEvent(onWSEvent);
  ws.setReconnectInterval(3000);   // auto-reconnect every 3s if dropped
  ws.enableHeartbeat(15000, 3000, 2); // ping every 15s, 3s timeout, 2 retries

  Serial.println("[WS] Connecting to wss://" + String(WS_HOST) + String(WS_PATH));

  // Boot sweep animation
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::DarkBlue; FastLED.show(); delay(12);
    leds[i] = CRGB::Black;
  }
  FastLED.show();
  Serial.println("[Boot] Ready ✓");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  ws.loop();          // handles connect/reconnect/ping automatically

  handleRFID();
  handleButtons();
  updateBlink();
}
