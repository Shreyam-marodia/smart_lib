# 📚 Smart Library

A home library system with LLM-powered book search, RFID tagging, and individually addressable LED slot indicators.

---

## Stack

| Layer | Tech |
|---|---|
| Server | Node.js + Express + WebSocket |
| Database | sql.js (SQLite in-memory, persisted to file) |
| AI Search | Claude API (claude-sonnet-4-6) |
| Frontend | Vanilla JS, dark library theme |
| ESP32 | Arduino + FastLED + MFRC522 + ArduinoWebsockets |
| LEDs | WS2812B strip |
| RFID | RC522 module |

---

## Setup

### 1. Replit / Server

1. Upload this project to Replit
2. Create a `.env` file (copy from `.env.example`):
   ```
   ANTHROPIC_API_KEY=your_key_here
   PORT=3000
   ```
3. Run:
   ```bash
   npm install
   npm start
   ```

### 2. ESP32

1. Open `esp32/smart_library.ino` in Arduino IDE
2. Install libraries via Library Manager:
   - `ArduinoWebsockets` by Gil Maimon
   - `MFRC522` by GithubCommunity
   - `FastLED` by Daniel Garcia
   - `ArduinoJson` by Benoit Blanchon
3. Edit these lines in the .ino file:
   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_SSID";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   const char* WS_URL        = "ws://YOUR_REPLIT_URL/?esp32=1";
   ```
4. Flash to ESP32

### 3. Wiring

**RC522 RFID → ESP32**
| RC522 | ESP32 |
|---|---|
| SDA | GPIO 5 |
| SCK | GPIO 18 |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| RST | GPIO 27 |
| 3.3V | 3.3V |
| GND | GND |

**WS2812B LED Strip → ESP32**
| LED | ESP32 |
|---|---|
| DATA | GPIO 4 |
| 5V | External 5V PSU |
| GND | GND (shared with ESP32) |

⚠️ Do NOT power the LED strip from the ESP32's 5V pin — use an external 5V 2A+ supply.

**Buttons → ESP32 (all INPUT_PULLUP)**
| Button | GPIO |
|---|---|
| UP | 12 |
| DOWN | 13 |
| LEFT | 14 |
| RIGHT | 15 |

Connect one leg to GPIO, other leg to GND.

---

## API Reference

| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/books` | List all books |
| GET | `/api/shelves` | List all shelves |
| GET | `/api/status` | Server health |
| POST | `/api/search` | `{ query }` → LLM book search |
| POST | `/api/books/register` | `{ rfid_uid, title, author, genre, shelf_id? }` |
| POST | `/api/books/:id/relocate` | Start ESP32 button navigation mode |
| POST | `/api/books/:id/move` | `{ shelf_id, row, col }` → direct move from UI |

---

## WebSocket Commands

### Server → ESP32
| cmd | Payload | Effect |
|---|---|---|
| `highlight_book` | `{ led_index, color }` | Blink LED at position |
| `highlight_new` | `{ led_index }` | Blue blink for new book placement |
| `start_relocation` | `{ book_id, current_led, shelf_id }` | Enter button nav mode |
| `relocation_confirmed` | `{ led_index }` | Flash green, exit nav mode |
| `slot_occupied` | `{ led_index }` | Flash red, stay in nav mode |
| `cancel` | — | Exit nav mode |

### ESP32 → Server
| cmd | Payload | Effect |
|---|---|---|
| `rfid_scan` | `{ rfid_uid }` | Lookup or register a book |
| `slot_selected` | `{ book_id, shelf_id, row, col, led_index }` | Confirm new slot via buttons |

---

## Shelf Layout

Default shelves (editable in `db/database.js`):

```
Shelf A: 3 rows × 10 cols, LEDs 0–29
Shelf B: 3 rows × 10 cols, LEDs 30–59
Shelf C: 3 rows × 10 cols, LEDs 60–89
```

LED index formula: `led_index = shelf.led_start + row * cols + col`

---

## Adding More Shelves

Edit the seed data in `db/database.js`:

```js
db.run(`INSERT INTO shelves (name, rows, cols, led_start, led_end) VALUES ('Shelf D', 3, 10, 90, 119)`)
```

And add a corresponding entry in `esp32/smart_library.ino`:

```cpp
const ShelfConfig SHELVES[] = {
  { "Shelf A", 3, 10, 0  },
  { "Shelf B", 3, 10, 30 },
  { "Shelf C", 3, 10, 60 },
  { "Shelf D", 3, 10, 90 },  // new
};
```

Also update `NUM_LEDS` in the .ino file.
