/*
 * Smart Library — ESP32 Firmware
 * ─────────────────────────────────────────────────────────
 * Hardware:
 *   - ESP32 DevKit
 *   - RC522 RFID module (SPI)
 *   - WS2812B LED strip (one continuous strip across all shelves)
 *   - 4 push buttons (UP/DOWN/LEFT/RIGHT) for slot navigation
 *
 * Libraries needed (install via Arduino Library Manager):
 *   - ArduinoWebsockets  (by Gil Maimon)
 *   - MFRC522            (by GithubCommunity)
 *   - FastLED            (by Daniel Garcia)
 *   - ArduinoJson        (by Benoit Blanchon)
 *
 * Wiring:
 *   RC522:  SDA=5, SCK=18, MOSI=23, MISO=19, RST=27, 3.3V, GND
 *   LEDs:   DATA=4, external 5V supply (not from ESP32!)
 *   Button UP:    GPIO 12 (INPUT_PULLUP)
 *   Button DOWN:  GPIO 13 (INPUT_PULLUP)
 *   Button LEFT:  GPIO 14 (INPUT_PULLUP)
 *   Button RIGHT: GPIO 15 (INPUT_PULLUP)
 */

#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <MFRC522.h>
#include <FastLED.h>
#include <SPI.h>

using namespace websockets;

// ─── Config ──────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* WS_URL        = "ws://YOUR_REPLIT_URL/?esp32=1";

// LED strip
#define LED_PIN       4
#define NUM_LEDS      90        // total across all shelves (match server config)
#define LED_BRIGHTNESS 80

// RFID
#define RFID_SS_PIN   5
#define RFID_RST_PIN  27

// Buttons
#define BTN_UP        12
#define BTN_DOWN      13
#define BTN_LEFT      14
#define BTN_RIGHT     15

#define DEBOUNCE_MS   50
#define HOLD_MS       2000

// Shelf layout (must match server DB)
// Each shelf: rows x cols
struct ShelfConfig {
  const char* name;
  uint8_t rows;
  uint8_t cols;
  uint8_t led_start;
};

const ShelfConfig SHELVES[] = {
  { "Shelf A", 3, 10, 0  },
  { "Shelf B", 3, 10, 30 },
  { "Shelf C", 3, 10, 60 },
};
const uint8_t NUM_SHELVES = 3;

// ─── State ───────────────────────────────────────────────────────────────────
CRGB leds[NUM_LEDS];
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
WebsocketsClient ws;

enum State { IDLE, NAVIGATING, CONFIRMING };
State state = IDLE;

// Navigation
int nav_shelf   = 0;
int nav_row     = 0;
int nav_col     = 0;
int nav_book_id = -1;

// Blink state
bool blink_on         = false;
unsigned long blink_last = 0;
int blink_led         = -1;
CRGB blink_color      = CRGB::Cyan;

// Highlight (non-blinking)
int highlight_led     = -1;
CRGB highlight_color  = CRGB::Black;

// ─── Button state ─────────────────────────────────────────────────────────────
struct Button {
  uint8_t pin;
  bool    last_state;
  unsigned long press_time;
  bool    hold_fired;
};

Button buttons[4] = {
  { BTN_UP,    HIGH, 0, false },
  { BTN_DOWN,  HIGH, 0, false },
  { BTN_LEFT,  HIGH, 0, false },
  { BTN_RIGHT, HIGH, 0, false },
};

// ─── RFID cooldown ───────────────────────────────────────────────────────────
unsigned long last_rfid_scan = 0;
#define RFID_COOLDOWN_MS 2000

// ─── WebSocket ───────────────────────────────────────────────────────────────
void sendJSON(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  ws.send(out);
  Serial.println("→ WS: " + out);
}

void handleWSMessage(WebsocketsMessage msg) {
  Serial.println("← WS: " + msg.data());

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, msg.data()) != DeserializationError::Ok) return;

  const char* cmd = doc["cmd"];

  if (strcmp(cmd, "highlight_book") == 0) {
    int led   = doc["led_index"];
    const char* color = doc["color"] | "green";
    startBlink(led, colorFromString(color));

  } else if (strcmp(cmd, "highlight_new") == 0) {
    int led = doc["led_index"];
    startBlink(led, CRGB::Blue);

  } else if (strcmp(cmd, "start_relocation") == 0) {
    nav_book_id = doc["book_id"];
    int shelf_id = doc["shelf_id"];     // 1-indexed from server
    int cur_led  = doc["current_led"];

    // Initialise nav to current position
    ledToNavCoords(cur_led, nav_shelf, nav_row, nav_col);
    state = NAVIGATING;

    startBlink(cur_led, CRGB::Yellow);
    Serial.printf("Navigation mode: book_id=%d shelf=%d\n", nav_book_id, nav_shelf);

  } else if (strcmp(cmd, "relocation_confirmed") == 0) {
    state = IDLE;
    int led = doc["led_index"];
    flashConfirm(led);

  } else if (strcmp(cmd, "slot_occupied") == 0) {
    // Flash red on that slot, stay in NAVIGATING
    int led = doc["led_index"];
    flashError(led);

  } else if (strcmp(cmd, "cancel") == 0) {
    state = IDLE;
    clearAll();
  }
}

// ─── LED Helpers ─────────────────────────────────────────────────────────────
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
  if (blink_led >= 0 && blink_led < NUM_LEDS) {
    leds[blink_led] = CRGB::Black;
    FastLED.show();
  }
  blink_led = -1;
}

void updateBlink() {
  if (blink_led < 0) return;
  unsigned long now = millis();
  if (now - blink_last >= 500) {
    blink_on = !blink_on;
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
    leds[led] = CRGB::Green;
    FastLED.show();
    delay(120);
    leds[led] = CRGB::Black;
    FastLED.show();
    delay(80);
  }
}

void flashError(int led) {
  for (int i = 0; i < 3; i++) {
    leds[led] = CRGB::Red;
    FastLED.show();
    delay(100);
    leds[led] = CRGB::Black;
    FastLED.show();
    delay(100);
  }
  // Resume blinking nav position
  startBlink(navToLED(nav_shelf, nav_row, nav_col), CRGB::Cyan);
}

// ─── Coordinate Helpers ───────────────────────────────────────────────────────
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
  // Fallback
  shelf = 0; row = 0; col = 0;
}

// ─── Navigation Logic ─────────────────────────────────────────────────────────
void moveNav(int drow, int dcol) {
  if (state != NAVIGATING) return;

  int new_row = nav_row + drow;
  int new_col = nav_col + dcol;
  int rows    = SHELVES[nav_shelf].rows;
  int cols    = SHELVES[nav_shelf].cols;

  // Column wrap with row carry-over
  if (new_col < 0) {
    new_row--;
    new_col = cols - 1;
  } else if (new_col >= cols) {
    new_row++;
    new_col = 0;
  }

  // Row clamping (stay on same shelf)
  if (new_row < 0)    new_row = 0;
  if (new_row >= rows) new_row = rows - 1;

  nav_row = new_row;
  nav_col = new_col;

  // Update blinking LED
  startBlink(navToLED(nav_shelf, nav_row, nav_col), CRGB::Cyan);

  Serial.printf("Nav: shelf=%d row=%d col=%d led=%d\n",
    nav_shelf, nav_row, nav_col, navToLED(nav_shelf, nav_row, nav_col));
}

void confirmNav() {
  if (state != NAVIGATING) return;
  state = CONFIRMING;

  int led = navToLED(nav_shelf, nav_row, nav_col);

  // Fast blink to indicate hold detected
  for (int i = 0; i < 6; i++) {
    leds[led] = CRGB::White;
    FastLED.show();
    delay(80);
    leds[led] = CRGB::Black;
    FastLED.show();
    delay(60);
  }

  // Send to server (shelf_id is 1-indexed on server)
  StaticJsonDocument<128> doc;
  doc["cmd"]      = "slot_selected";
  doc["book_id"]  = nav_book_id;
  doc["shelf_id"] = nav_shelf + 1;   // server uses 1-indexed
  doc["row"]      = nav_row;
  doc["col"]      = nav_col;
  doc["led_index"]= led;
  sendJSON(doc);
}

// ─── Button Handling ──────────────────────────────────────────────────────────
void handleButtons() {
  for (int i = 0; i < 4; i++) {
    Button& btn = buttons[i];
    bool reading = digitalRead(btn.pin);

    if (reading == LOW && btn.last_state == HIGH) {
      // Pressed
      btn.press_time = millis();
      btn.hold_fired = false;
    }

    if (reading == LOW && !btn.hold_fired) {
      if (millis() - btn.press_time >= HOLD_MS) {
        btn.hold_fired = true;
        // Hold = confirm
        confirmNav();
      }
    }

    if (reading == HIGH && btn.last_state == LOW) {
      // Released
      if (!btn.hold_fired && millis() - btn.press_time >= DEBOUNCE_MS) {
        // Normal press
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

// ─── RFID Scanning ───────────────────────────────────────────────────────────
void handleRFID() {
  if (state != IDLE) return;
  if (millis() - last_rfid_scan < RFID_COOLDOWN_MS) return;
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  // Build UID string
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (i > 0) uid += ":";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  Serial.println("RFID scan: " + uid);
  last_rfid_scan = millis();

  // Send to server
  StaticJsonDocument<128> doc;
  doc["cmd"]      = "rfid_scan";
  doc["rfid_uid"] = uid;
  sendJSON(doc);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ─── WiFi & WebSocket ────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
}

void connectWS() {
  ws.onMessage(handleWSMessage);
  ws.onEvent([](WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened)
      Serial.println("WS connected");
    if (event == WebsocketsEvent::ConnectionClosed)
      Serial.println("WS disconnected — reconnecting…");
  });

  while (!ws.connect(WS_URL)) {
    Serial.println("WS connect failed, retrying…");
    delay(2000);
  }
  Serial.println("WS connected to server");
}

// ─── Setup & Loop ────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

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
  Serial.println("RFID ready");

  // Network
  connectWiFi();
  connectWS();

  // Boot animation: sweep once
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::DarkBlue;
    FastLED.show();
    delay(15);
    leds[i] = CRGB::Black;
  }
  FastLED.show();
  Serial.println("✅ Smart Library ready");
}

void loop() {
  // Keep WebSocket alive, reconnect if dropped
  if (!ws.available()) {
    Serial.println("WS dropped, reconnecting…");
    delay(2000);
    connectWS();
  }
  ws.poll();

  handleRFID();
  handleButtons();
  updateBlink();
}
