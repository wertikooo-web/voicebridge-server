const express = require("express");
const fs = require("fs");
const http = require("http");
const os = require("os");
const WebSocket = require("ws");
const path = require("path");
const crypto = require("crypto");

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });
const HEARTBEAT_INTERVAL_MS = 10000;
const PORT = process.env.PORT || 3000;
const AUDIO_DIR = path.join(__dirname, "public", "audio");

fs.mkdirSync(AUDIO_DIR, { recursive: true });

app.use((req, res, next) => {
  if (req.path === "/" || req.path.endsWith(".html") || req.path === "/sw.js") {
    res.setHeader("Cache-Control", "no-store, no-cache, must-revalidate, proxy-revalidate");
    res.setHeader("Pragma", "no-cache");
    res.setHeader("Expires", "0");
  }
  next();
});

app.use(express.static(path.join(__dirname, "public")));
app.use("/vendor/three", express.static(path.join(__dirname, "node_modules", "three")));
app.use(
  "/generated-room",
  express.static(path.join(process.env.USERPROFILE || "C:\\Users\\user", ".codex", "generated_images"))
);

const senders = new Set();
let phoneB = null;

/* PATCH:liveLineState */
const LIVE_LINE_TIMEOUT_MS = 3 * 60 * 1000;
const liveLineState = { active: false, timer: null };

function endLiveLine(reason) {
  if (!liveLineState.active) return;
  liveLineState.active = false;
  if (liveLineState.timer) { clearTimeout(liveLineState.timer); liveLineState.timer = null; }
  const msg = { type: 'live_line_ended', reason };
  broadcastToSenders(msg);
  if (phoneB) safeSend(phoneB, msg);
  console.log('live_line ended:', reason);
}

function startLiveLineTimer() {
  if (liveLineState.timer) clearTimeout(liveLineState.timer);
  liveLineState.timer = setTimeout(() => endLiveLine('timeout'), LIVE_LINE_TIMEOUT_MS);
}

function getLanAddress() {
  const interfaces = os.networkInterfaces();
  for (const entries of Object.values(interfaces)) {
    for (const entry of entries || []) {
      if (entry.family === "IPv4" && !entry.internal) return entry.address;
    }
  }
  return "localhost";
}

function publicBaseUrl(ws) {
  if (process.env.PUBLIC_BASE_URL) return process.env.PUBLIC_BASE_URL.replace(/\/$/, "");
  const host = ws?.requestHost || "";
  const proto = ws?.requestProto || "http";
  if (host && !host.startsWith("localhost") && !host.startsWith("127.0.0.1")) return `${proto}://${host}`;
  return `http://${getLanAddress()}:${PORT}`;
}

function saveAudioFiles(data, ws) {
  if (!data.audio && !data.pcm) return {};

  const messageId = data.message_id || `voice-${Date.now()}-${crypto.randomBytes(4).toString("hex")}`;
  const result = {
    message_id: messageId,
    format: data.audioFormat || "pcm_s16le",
    sampleRate: data.sampleRate || 16000,
    channels: data.channels || 1,
    bitsPerSample: data.bitsPerSample || 16
  };

  if (data.audio) {
    const wavName = `${messageId}.wav`;
    fs.writeFileSync(path.join(AUDIO_DIR, wavName), Buffer.from(data.audio, "base64"));
    result.audio_url = `${publicBaseUrl(ws)}/audio/${wavName}`;
  }

  if (data.pcm) {
    const pcmName = `${messageId}.pcm`;
    fs.writeFileSync(path.join(AUDIO_DIR, pcmName), Buffer.from(data.pcm, "base64"));
    result.pcm_url = `${publicBaseUrl(ws)}/audio/${pcmName}`;
  }

  return result;
}

function connectionStatus() {
  return {
    type: "connection_status",
    senderConnected: senders.size > 0,
    senderCount: senders.size,
    receiverConnected: Boolean(phoneB && phoneB.readyState === WebSocket.OPEN),
    totalClients: wss.clients.size
  };
}

function broadcastStatus() {
  const status = connectionStatus();
  wss.clients.forEach((client) => {
    safeSend(client, status);
  });
}

app.get("/api/status", (req, res) => {
  res.json(connectionStatus());
});

function cleanupClient(ws) {
  const changed = senders.delete(ws) || ws === phoneB;
  if (ws === phoneB) phoneB = null;
  if (changed) setTimeout(broadcastStatus, 0);
}

function replaceClient(currentClient, nextClient, roleName) {
  if (currentClient && currentClient !== nextClient) {
    console.log(`Replacing stale ${roleName} connection`);
    currentClient.terminate();
  }
  return nextClient;
}

function safeSend(ws, data) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    cleanupClient(ws);
    return false;
  }

  ws.send(JSON.stringify(data), (error) => {
    if (error) {
      console.log("Send failed, terminating stale connection:", error.message);
      cleanupClient(ws);
      ws.terminate();
    }
  });

  return true;
}

function broadcastToSenders(data, excludeWs) {
  let delivered = 0;
  senders.forEach((sender) => {
    if (sender !== excludeWs && safeSend(sender, data)) delivered += 1;
  });
  return delivered;
}

wss.on("connection", (ws, req) => {
  console.log("Client connected");
  ws.isAlive = true;
  ws.requestHost = req.headers.host;
  ws.requestProto = (req.headers["x-forwarded-proto"] || (req.socket.encrypted ? "https" : "http")).split(",")[0].trim();

  ws.on("pong", () => {
    ws.isAlive = true;
  });

  ws.on("message", (message) => {
    let data;
    try {
      data = JSON.parse(message);
    } catch {
      return;
    }

    if (data.type === "register" && data.role === "sender") {
      senders.add(ws);
      ws.role = "sender";
      console.log(`Sender registered (${senders.size} total)`);
      broadcastStatus();
    }
    if (data.type === "register" && data.role === "receiver") {
      phoneB = replaceClient(phoneB, ws, "receiver");
      ws.role = "receiver";
      console.log("Receiver registered");
      broadcastStatus();
    }
    if (data.type === "register" && data.role === "scheduler") {
      console.log("Scheduler registered");
    }
    if (data.type === "keepalive") {
      /* ESP32 keep-alive ping — соединение живое, Railway не режет */
      ws.isAlive = true;
    }
    if (data.type === "audio_message" && phoneB) {
      const text = data.text || data.transcript || "";
      const audioFiles = saveAudioFiles(data, ws);
      console.log("audio_message -> receiver as voice_message", text ? `(${text.length} chars)` : "(no text)", audioFiles.audio_url || audioFiles.pcm_url || "(no audio url)");
      safeSend(phoneB, {
        type: "voice_message",
        text,
        transcript: text,
        ...audioFiles,
        audio: data.audio,
        mimeType: data.mimeType || "audio/wav",
        hasAudio: Boolean(data.audio),
        originalType: "audio_message"
      });
      broadcastToSenders({
        type: "activity_event",
        activity: {
          msg: text || "Голосовое сообщение",
          icon: "🎤",
          type: "voice",
          sender: "Ты",
          duration: data.duration || data.durationSeconds || null
        }
      });
    }
    if (data.type === "phrase_message") {
      console.log("phrase_message -> receiver", data.text || "");
      if (phoneB) safeSend(phoneB, data);
      safeSend(ws, { type: "phrase_sent", text: data.text });
      broadcastToSenders({
        type: "activity_event",
        activity: {
          msg: data.text || "Фраза отправлена",
          icon: "💬",
          type: "phrase",
          sender: "Ты"
        }
      });
    }
    if (data.type === "direct_line_request" && phoneB) {
      console.log("direct_line_request -> receiver");
      safeSend(phoneB, { type: "direct_line_request" });
    }
    if (data.type === "direct_line_cancel" && phoneB) {
      console.log("direct_line_cancel -> receiver");
      safeSend(phoneB, { type: "direct_line_cancel" });
    }
    if (data.type === "direct_line_accept" && senders.size) {
      console.log(`direct_line_accept -> ${senders.size} sender(s)`);
      broadcastToSenders({ type: "direct_line_accept" });
    }
    if (data.type === "direct_line_timeout" && senders.size) {
      console.log(`direct_line_timeout -> ${senders.size} sender(s)`);
      broadcastToSenders({ type: "direct_line_timeout" });
    }
    if (data.type === "profile_sync" && phoneB) {
      console.log("profile_sync -> receiver");
      safeSend(phoneB, data);
    }
    if (data.type === "reply_message" && senders.size) {
      console.log(`reply_message -> ${senders.size} sender(s)`);
      broadcastToSenders(data);
    }
    if (data.type === "dad_wants_to_talk" && senders.size) {
      console.log(`dad_wants_to_talk -> ${senders.size} sender(s)`);
      broadcastToSenders({ type: "dad_wants_to_talk" });
    }
    if (data.type === "wants_to_talk" && senders.size) {
      console.log(`wants_to_talk -> ${senders.size} sender(s)`);
      broadcastToSenders({ type: "dad_wants_to_talk" });
    }
    if (data.type === "help_request" && senders.size) {
      console.log(`help_request -> ${senders.size} sender(s)`);
      broadcastToSenders({ type: "help_request" });
    }
    if (data.type === "sos_acknowledged" && phoneB) {
      console.log("sos_acknowledged -> receiver");
      safeSend(phoneB, { type: "sos_acknowledged" });
    }
    if (data.type === "lunara_start" && senders.size) {
      console.log(`lunara_start -> ${senders.size} sender(s)`);
      broadcastToSenders({ type: "lunara_start" });
    }
    /* PATCH:liveLineHandlers */
    if (data.type === 'live_line_start') {
      if (phoneB) {
        liveLineState.active = true;
        startLiveLineTimer();
        safeSend(phoneB, { type: 'live_line_start' });
        broadcastToSenders({ type: 'live_line_started' });
        console.log('live_line_start -> receiver');
      }
    }
    if (data.type === 'live_audio_chunk') {
      if (!liveLineState.active) return;
      if (ws.role === 'sender' && phoneB) { safeSend(phoneB, data); }
      else if (ws.role === 'receiver') { broadcastToSenders(data); }
    }
    /* PATCH:liveWarning */
    if (data.type === 'live_line_warning') {
      if (ws.role === 'sender' && phoneB) safeSend(phoneB, {type:'live_line_warning'});
    }
    if (data.type === 'live_line_end') {
      endLiveLine(data.reason || 'user');
    }
    if (data.type === 'dad_voice_message' && senders.size) {
      const audioFiles = saveAudioFiles(data, ws);
      console.log('dad_voice_message ->', audioFiles.audio_url || '(no url)');
      broadcastToSenders({
        type: 'dad_voice_message',
        displayName: data.displayName || 'Папа',
        duration: data.duration || null,
        ...audioFiles,
        audio: data.audio,
        mimeType: data.mimeType || 'audio/wav',
      });
    }
    if (data.type === 'remind_later_ack' && phoneB) {
      safeSend(phoneB, { type: 'remind_later_ack', minutes: data.minutes });
    }
  });

  ws.on("error", (error) => {
    console.log("WebSocket error:", error.message);
    cleanupClient(ws);
  });

  ws.on("close", () => {
    cleanupClient(ws);
    console.log("Client disconnected");
  });
});

const heartbeat = setInterval(() => {
  wss.clients.forEach((ws) => {
    if (!ws.isAlive) {
      console.log("Terminating stale client");
      cleanupClient(ws);
      ws.terminate();
      return;
    }

    ws.isAlive = false;
    ws.ping();
  });
}, HEARTBEAT_INTERVAL_MS);

wss.on("close", () => {
  clearInterval(heartbeat);
});

server.listen(PORT, () => {
  console.log(`VoiceBridge test server running on http://localhost:${PORT}`);
});
