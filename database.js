const initSqlJs = require("sql.js");
const fs = require("fs");
const path = require("path");

const DB_PATH = process.env.DB_PATH || path.join(__dirname, "..", "library.db");

let db = null;

async function initDB() {
  const SQL = await initSqlJs();

  if (fs.existsSync(DB_PATH)) {
    const fileBuffer = fs.readFileSync(DB_PATH);
    db = new SQL.Database(fileBuffer);
  } else {
    db = new SQL.Database();
  }

  db.run(`
    CREATE TABLE IF NOT EXISTS shelves (
      id        INTEGER PRIMARY KEY AUTOINCREMENT,
      name      TEXT NOT NULL,
      rows      INTEGER NOT NULL,
      cols      INTEGER NOT NULL,
      led_start INTEGER NOT NULL,
      led_end   INTEGER NOT NULL
    );
  `);

  db.run(`
    CREATE TABLE IF NOT EXISTS books (
      id           INTEGER PRIMARY KEY AUTOINCREMENT,
      rfid_uid     TEXT UNIQUE NOT NULL,
      title        TEXT NOT NULL,
      author       TEXT NOT NULL,
      genre        TEXT NOT NULL,
      shelf_id     INTEGER NOT NULL,
      shelf_row    INTEGER NOT NULL,
      shelf_col    INTEGER NOT NULL,
      led_index    INTEGER NOT NULL,
      added_at     DATETIME DEFAULT CURRENT_TIMESTAMP,
      last_moved_at DATETIME DEFAULT CURRENT_TIMESTAMP,
      FOREIGN KEY (shelf_id) REFERENCES shelves(id)
    );
  `);

  db.run(`
    CREATE TABLE IF NOT EXISTS moves_log (
      id        INTEGER PRIMARY KEY AUTOINCREMENT,
      book_id   INTEGER NOT NULL,
      from_led  INTEGER,
      to_led    INTEGER NOT NULL,
      moved_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
      FOREIGN KEY (book_id) REFERENCES books(id)
    );
  `);

  // Seed default shelves if none exist
  const result = db.exec("SELECT COUNT(*) as count FROM shelves");
  const count = result[0].values[0][0];
  if (count === 0) {
    db.run(`INSERT INTO shelves (name, rows, cols, led_start, led_end) VALUES ('Shelf A', 3, 10, 0, 29)`);
    db.run(`INSERT INTO shelves (name, rows, cols, led_start, led_end) VALUES ('Shelf B', 3, 10, 30, 59)`);
    db.run(`INSERT INTO shelves (name, rows, cols, led_start, led_end) VALUES ('Shelf C', 3, 10, 60, 89)`);
  }

  saveDB();
  console.log("✅ Database initialized");
  return db;
}

function saveDB() {
  if (!db) return;
  const data = db.export();
  fs.writeFileSync(DB_PATH, Buffer.from(data));
}

// ─── Shelf Helpers ────────────────────────────────────────────────────────────

function getShelves() {
  const res = db.exec("SELECT * FROM shelves");
  if (!res.length) return [];
  const [cols, rows] = [res[0].columns, res[0].values];
  return rows.map((r) => Object.fromEntries(cols.map((c, i) => [c, r[i]])));
}

function getShelfById(id) {
  const res = db.exec(`SELECT * FROM shelves WHERE id = ${id}`);
  if (!res.length || !res[0].values.length) return null;
  const cols = res[0].columns;
  return Object.fromEntries(cols.map((c, i) => [c, res[0].values[0][i]]));
}

// ─── Book Helpers ─────────────────────────────────────────────────────────────

function getBooks() {
  const res = db.exec(`
    SELECT b.*, s.name as shelf_name 
    FROM books b JOIN shelves s ON b.shelf_id = s.id
    ORDER BY b.shelf_id, b.led_index
  `);
  if (!res.length) return [];
  const [cols, rows] = [res[0].columns, res[0].values];
  return rows.map((r) => Object.fromEntries(cols.map((c, i) => [c, r[i]])));
}

function getBookByRFID(rfid_uid) {
  const res = db.exec(`SELECT * FROM books WHERE rfid_uid = '${rfid_uid}'`);
  if (!res.length || !res[0].values.length) return null;
  const cols = res[0].columns;
  return Object.fromEntries(cols.map((c, i) => [c, res[0].values[0][i]]));
}

function getBookById(id) {
  const res = db.exec(`SELECT * FROM books WHERE id = ${id}`);
  if (!res.length || !res[0].values.length) return null;
  const cols = res[0].columns;
  return Object.fromEntries(cols.map((c, i) => [c, res[0].values[0][i]]));
}

// Returns all occupied LED indices on a shelf
function getOccupiedSlots(shelf_id) {
  const res = db.exec(`SELECT shelf_row, shelf_col, led_index FROM books WHERE shelf_id = ${shelf_id}`);
  if (!res.length) return [];
  return res[0].values.map((r) => ({ row: r[0], col: r[1], led_index: r[2] }));
}

// ─── Slot Allocation ──────────────────────────────────────────────────────────

function allocateSlot(shelf_id) {
  const shelf = getShelfById(shelf_id);
  if (!shelf) return null;

  const occupied = getOccupiedSlots(shelf_id);
  const occupiedLEDs = new Set(occupied.map((o) => o.led_index));

  for (let row = 0; row < shelf.rows; row++) {
    for (let col = 0; col < shelf.cols; col++) {
      const led_index = shelf.led_start + row * shelf.cols + col;
      if (led_index > shelf.led_end) break;
      if (!occupiedLEDs.has(led_index)) {
        return { row, col, led_index, shelf_id };
      }
    }
  }
  return null; // shelf full
}

function findLeastFullShelf() {
  const shelves = getShelves();
  let best = null;
  let bestFree = -1;

  for (const shelf of shelves) {
    const total = shelf.rows * shelf.cols;
    const occupied = getOccupiedSlots(shelf.id).length;
    const free = total - occupied;
    if (free > bestFree) {
      bestFree = free;
      best = shelf;
    }
  }
  return best;
}

function isSlotFree(shelf_id, row, col) {
  const shelf = getShelfById(shelf_id);
  if (!shelf) return false;
  const led_index = shelf.led_start + row * shelf.cols + col;
  const res = db.exec(`SELECT id FROM books WHERE led_index = ${led_index} AND shelf_id = ${shelf_id}`);
  return !res.length || !res[0].values.length;
}

// ─── Write Operations ─────────────────────────────────────────────────────────

function addBook({ rfid_uid, title, author, genre, shelf_id, shelf_row, shelf_col, led_index }) {
  db.run(
    `INSERT INTO books (rfid_uid, title, author, genre, shelf_id, shelf_row, shelf_col, led_index)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
    [rfid_uid, title, author, genre, shelf_id, shelf_row, shelf_col, led_index]
  );
  saveDB();

  // log the initial placement
  const newBook = getBookByRFID(rfid_uid);
  db.run(`INSERT INTO moves_log (book_id, from_led, to_led) VALUES (?, NULL, ?)`, [newBook.id, led_index]);
  saveDB();

  return newBook;
}

function moveBook(book_id, shelf_id, shelf_row, shelf_col, led_index) {
  const book = getBookById(book_id);
  if (!book) return null;

  db.run(
    `UPDATE books SET shelf_id=?, shelf_row=?, shelf_col=?, led_index=?, last_moved_at=CURRENT_TIMESTAMP WHERE id=?`,
    [shelf_id, shelf_row, shelf_col, led_index, book_id]
  );

  db.run(`INSERT INTO moves_log (book_id, from_led, to_led) VALUES (?, ?, ?)`, [
    book_id,
    book.led_index,
    led_index,
  ]);

  saveDB();
  return getBookById(book_id);
}

// ─── Search Context Builder ───────────────────────────────────────────────────

function buildBookContext() {
  const books = getBooks();
  if (!books.length) return "The library is currently empty.";
  return books
    .map(
      (b) =>
        `ID:${b.id} | "${b.title}" by ${b.author} | Genre: ${b.genre} | ${b.shelf_name} Row ${b.shelf_row + 1} Col ${b.shelf_col + 1} | LED:${b.led_index}`
    )
    .join("\n");
}

module.exports = {
  initDB,
  saveDB,
  getShelves,
  getShelfById,
  getBooks,
  getBookByRFID,
  getBookById,
  getOccupiedSlots,
  allocateSlot,
  findLeastFullShelf,
  isSlotFree,
  addBook,
  moveBook,
  buildBookContext,
};
