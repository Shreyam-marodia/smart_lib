const WebSocket = require("ws");

let wss = null;
const clients = new Map(); // id → { ws, type: 'esp32' | 'browser' }
let clientIdCounter = 0;

function initWebSocket(server) {
  wss = new WebSocket.Server({ server });

  wss.on("connection", (ws, req) => {
    const id = ++clientIdCounter;
    const type = req.url?.includes("esp32") ? "esp32" : "browser";
    clients.set(id, { ws, type });
    console.log(`🔌 [WS] ${type} connected (id=${id})`);

    ws.on("message", (raw) => {
      try {
        const msg = JSON.parse(raw.toString());
        console.log(`📨 [WS] from ${type}:`, msg);
        handleMessage(id, type, msg, ws);
      } catch (e) {
        console.error("[WS] Bad JSON:", raw.toString());
      }
    });

    ws.on("close", () => {
      clients.delete(id);
      console.log(`🔴 [WS] ${type} disconnected (id=${id})`);
    });

    ws.on("error", (err) => {
      console.error(`[WS] Error on client ${id}:`, err.message);
    });

    // Send initial handshake
    send(ws, { cmd: "handshake", role: type, id });
  });

  console.log("✅ WebSocket server ready");
}

// ─── Message Router ───────────────────────────────────────────────────────────
// Import handler is set externally to avoid circular deps
let messageHandler = null;

function setMessageHandler(fn) {
  messageHandler = fn;
}

function handleMessage(id, type, msg, ws) {
  if (messageHandler) {
    messageHandler(id, type, msg, ws);
  }
}

// ─── Send Helpers ─────────────────────────────────────────────────────────────

function send(ws, payload) {
  if (ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(payload));
  }
}

function broadcastToESP32(payload) {
  for (const [, client] of clients) {
    if (client.type === "esp32") {
      send(client.ws, payload);
    }
  }
}

function broadcastToBrowsers(payload) {
  for (const [, client] of clients) {
    if (client.type === "browser") {
      send(client.ws, payload);
    }
  }
}

function broadcastAll(payload) {
  for (const [, client] of clients) {
    send(client.ws, payload);
  }
}

function getConnectedESP32Count() {
  let count = 0;
  for (const [, c] of clients) if (c.type === "esp32") count++;
  return count;
}

module.exports = {
  initWebSocket,
  setMessageHandler,
  send,
  broadcastToESP32,
  broadcastToBrowsers,
  broadcastAll,
  getConnectedESP32Count,
  clients,
};
