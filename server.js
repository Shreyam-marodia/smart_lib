require("dotenv").config();

const express = require("express");
const http = require("http");
const path = require("path");

const db = require("./db/database");
const ws = require("./ws/websocket");
const { searchBooks } = require("./ai/search");

const app = express();
const server = http.createServer(app);

app.use(express.json());
app.use(express.static(path.join(__dirname, "public")));
// Explicitly serve the frontend when visiting the root URL
app.get("/", (req, res) => {
  res.sendFile(path.join(__dirname, "public", "index.html"), (err) => {
    if (err) {
      console.error("Error serving index.html:", err);
      res.status(500).send("index.html not found in the public folder!");
    }
  });
});

// ─── WebSocket Message Handler ────────────────────────────────────────────────

ws.setMessageHandler((id, type, msg, clientWs) => {
  switch (msg.cmd) {
    // ── ESP32: RFID scanned ──────────────────────────────────────────────────
    case "rfid_scan": {
      const { rfid_uid } = msg;
      const book = db.getBookByRFID(rfid_uid);

      if (book) {
        // Known book: light up its current position (return mode)
        ws.send(clientWs, {
          cmd: "highlight_book",
          led_index: book.led_index,
          color: "yellow",
          book_id: book.id,
          title: book.title,
        });

        // Tell browsers too
        ws.broadcastToBrowsers({
          cmd: "book_found",
          book,
        });
      } else {
        // Unknown: register mode
        ws.send(clientWs, { cmd: "unknown_rfid", rfid_uid });
        ws.broadcastToBrowsers({ cmd: "register_prompt", rfid_uid });
      }
      break;
    }

    // ── ESP32: slot selected via buttons ─────────────────────────────────────
    case "slot_selected": {
      const { book_id, shelf_id, row, col } = msg;
      const shelf = db.getShelfById(shelf_id);
      if (!shelf) {
        ws.send(clientWs, { cmd: "error", message: "Invalid shelf" });
        break;
      }

      const led_index = shelf.led_start + row * shelf.cols + col;
      const free = db.isSlotFree(shelf_id, row, col);

      if (!free) {
        ws.send(clientWs, { cmd: "slot_occupied", led_index, row, col });
      } else {
        // Confirm move
        const updated = db.moveBook(book_id, shelf_id, row, col, led_index);
        ws.send(clientWs, { cmd: "relocation_confirmed", led_index, book: updated });
        ws.broadcastToBrowsers({ cmd: "book_moved", book: updated });
      }
      break;
    }

    default:
      console.log(`[WS] Unhandled cmd: ${msg.cmd}`);
  }
});

// ─── REST API ─────────────────────────────────────────────────────────────────

// GET /api/books — list all books
app.get("/api/books", (req, res) => {
  res.json(db.getBooks());
});

// GET /api/shelves — list shelves
app.get("/api/shelves", (req, res) => {
  res.json(db.getShelves());
});

// POST /api/search — LLM book search
app.post("/api/search", async (req, res) => {
  const { query } = req.body;
  if (!query?.trim()) return res.status(400).json({ error: "query is required" });

  try {
    const bookContext = db.buildBookContext();
    const result = await searchBooks(query, bookContext);

    // For each match, tell ESP32 to highlight the LED
    if (result.matches?.length > 0) {
      const top = result.matches[0];
      ws.broadcastToESP32({
        cmd: "highlight_book",
        led_index: top.led_index,
        color: "green",
        book_id: top.id,
      });
    }

    res.json(result);
  } catch (err) {
    console.error("[Search] Error:", err.message);
    res.status(500).json({ error: "Search failed", detail: err.message });
  }
});

// POST /api/books/register — register a new book
app.post("/api/books/register", (req, res) => {
  const { rfid_uid, title, author, genre, shelf_id } = req.body;

  if (!rfid_uid || !title || !author || !genre) {
    return res.status(400).json({ error: "rfid_uid, title, author, genre are required" });
  }

  // Check for duplicate RFID
  if (db.getBookByRFID(rfid_uid)) {
    return res.status(409).json({ error: "Book with this RFID already registered" });
  }

  // Auto-allocate shelf
  const targetShelf = shelf_id ? db.getShelfById(shelf_id) : db.findLeastFullShelf();
  if (!targetShelf) return res.status(500).json({ error: "No shelves available" });

  const slot = db.allocateSlot(targetShelf.id);
  if (!slot) return res.status(507).json({ error: "Shelf is full" });

  const book = db.addBook({
    rfid_uid,
    title,
    author,
    genre,
    shelf_id: targetShelf.id,
    shelf_row: slot.row,
    shelf_col: slot.col,
    led_index: slot.led_index,
  });

  // Tell ESP32 to flash the assigned slot
  ws.broadcastToESP32({
    cmd: "highlight_new",
    led_index: slot.led_index,
    color: "blue",
    book_id: book.id,
  });

  res.status(201).json({ book, slot });
});

// POST /api/books/:id/relocate — start relocation mode
app.post("/api/books/:id/relocate", (req, res) => {
  const book = db.getBookById(parseInt(req.params.id));
  if (!book) return res.status(404).json({ error: "Book not found" });

  // Tell ESP32 to enter navigation mode
  ws.broadcastToESP32({
    cmd: "start_relocation",
    book_id: book.id,
    current_led: book.led_index,
    shelf_id: book.shelf_id,
  });

  // Highlight current position in yellow
  ws.broadcastToESP32({
    cmd: "highlight_book",
    led_index: book.led_index,
    color: "yellow",
    book_id: book.id,
  });

  res.json({ message: "Relocation mode started", book });
});

// POST /api/books/:id/move — direct move (from UI manual grid)
app.post("/api/books/:id/move", (req, res) => {
  const { shelf_id, row, col } = req.body;
  const book_id = parseInt(req.params.id);

  const shelf = db.getShelfById(shelf_id);
  if (!shelf) return res.status(400).json({ error: "Invalid shelf" });

  if (!db.isSlotFree(shelf_id, row, col)) {
    return res.status(409).json({ error: "Slot is already occupied" });
  }

  const led_index = shelf.led_start + row * shelf.cols + col;
  const updated = db.moveBook(book_id, shelf_id, row, col, led_index);

  ws.broadcastToESP32({
    cmd: "relocation_confirmed",
    led_index,
    book: updated,
  });

  res.json({ book: updated });
});

// GET /api/books/:id — single book
app.get("/api/books/:id", (req, res) => {
  const book = db.getBookById(parseInt(req.params.id));
  if (!book) return res.status(404).json({ error: "Not found" });
  res.json(book);
});

// GET /api/status — server health
app.get("/api/status", (req, res) => {
  res.json({
    status: "ok",
    esp32_connected: ws.getConnectedESP32Count(),
    books: db.getBooks().length,
    shelves: db.getShelves().length,
  });
});

// ─── Start ────────────────────────────────────────────────────────────────────

async function start() {
  await db.initDB();
  ws.initWebSocket(server);

  const PORT = process.env.PORT || 3000;
  server.listen(PORT,"0.0.0.0", () => {
    console.log(`\n📚 Smart Library running at http://localhost:${PORT}`);
    console.log(`🔌 WebSocket: ws://localhost:${PORT}`);
    console.log(`   ESP32 connects to: ws://localhost:${PORT}/?esp32=1\n`);
  });
}

start().catch(console.error);
