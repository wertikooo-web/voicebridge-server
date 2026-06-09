const express = require("express");
const fs = require("fs");
const http = require("http");
const os = require("os");
const WebSocket = require("ws");
const path = require("path");
const crypto = require("crypto");
const webpush = require("web-push");

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });
const HEARTBEAT_INTERVAL_MS = Number(process.env.WS_HEARTBEAT_INTERVAL_MS || 25000);
const HEARTBEAT_TIMEOUT_MS = Number(process.env.WS_HEARTBEAT_TIMEOUT_MS || 90000);
const PORT = process.env.PORT || 3000;
const AUDIO_DIR = path.join(__dirname, "public", "audio");
const VAPID_PUBLIC_KEY = process.env.VAPID_PUBLIC_KEY || "";
const VAPID_PRIVATE_KEY = process.env.VAPID_PRIVATE_KEY || "";
const VAPID_SUBJECT = process.env.VAPID_SUBJECT || "mailto:support@voicebridge.local";
const pushSubscriptions = new Map();

fs.mkdirSync(AUDIO_DIR, { recursive: true });

if (VAPID_PUBLIC_KEY && VAPID_PRIVATE_KEY) {
  webpush.setVapidDetails(VAPID_SUBJECT, VAPID_PUBLIC_KEY, VAPID_PRIVATE_KEY);
} else {
  console.warn("Web Push disabled: set VAPID_PUBLIC_KEY and VAPID_PRIVATE_KEY");
}

app.use(express.json({ limit: "1mb" }));

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
const receivers = new Set();
let phoneB = null;
let clientSeq = 0;

/* PATCH:liveLineState */
const LIVE_LINE_TIMEOUT_MS = 3 * 60 * 1000;
const liveLineState = { active: false, timer: null };

function endLiveLine(reason) {
  if (!liveLineState.active) return;
  liveLineState.active = false;
  if (liveLineState.timer) { clearTimeout(liveLineState.timer); liveLineState.timer = null; }
  const msg = { type: 'live_line_ended', reason };
  broadcastToSenders(msg);
  sendToReceivers(msg);
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
    receiverConnected: receivers.size > 0,
    receiverCount: receivers.size,
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

app.get("/api/push/vapid-public-key", (req, res) => {
  if (!VAPID_PUBLIC_KEY) return res.status(503).json({ error: "VAPID public key is not configured" });
  res.json({ publicKey: VAPID_PUBLIC_KEY });
});

app.get("/api/push/status", (req, res) => {
  res.json({
    enabled: Boolean(VAPID_PUBLIC_KEY && VAPID_PRIVATE_KEY),
    subscriptions: pushSubscriptions.size
  });
});

app.post("/api/push/subscribe", (req, res) => {
  const subscription = req.body;
  if (!subscription || !subscription.endpoint) {
    return res.status(400).json({ error: "Invalid push subscription" });
  }

  pushSubscriptions.set(subscription.endpoint, subscription);
  console.log(`Push subscription saved (${pushSubscriptions.size} total)`);
  res.json({ ok: true, subscriptions: pushSubscriptions.size });
});

app.post("/api/push/unsubscribe", (req, res) => {
  const endpoint = req.body?.endpoint;
  if (endpoint) pushSubscriptions.delete(endpoint);
  res.json({ ok: true, subscriptions: pushSubscriptions.size });
});

async function sendPushToAll(payload) {
  if (!VAPID_PUBLIC_KEY || !VAPID_PRIVATE_KEY || pushSubscriptions.size === 0) return;
  const enrichedPayload = {
    ...payload,
    pushId: payload.pushId || crypto.randomUUID(),
    sentAt: Date.now()
  };
  const body = JSON.stringify(enrichedPayload);
  console.log(`Push sending ${enrichedPayload.type || "unknown"} to ${pushSubscriptions.size} subscription(s)`);
  const tasks = Array.from(pushSubscriptions.entries()).map(async ([endpoint, subscription]) => {
    try {
      await webpush.sendNotification(subscription, body);
      console.log(`Push sent ${payload.type || "unknown"}`);
    } catch (error) {
      if (error.statusCode === 404 || error.statusCode === 410) {
        pushSubscriptions.delete(endpoint);
      }
      console.log("Push send failed:", error.statusCode || "", error.message);
    }
  });
  await Promise.allSettled(tasks);
}

function notifyDadWantsToTalk() {
  return sendPushToAll({
    type: "dad_wants_to_talk",
    title: "\uD83D\uDC4B \u0425\u043E\u0447\u0435\u0442 \u043F\u043E\u0433\u043E\u0432\u043E\u0440\u0438\u0442\u044C",
    body: "\u041F\u0430\u043F\u0430 \u0445\u043E\u0447\u0435\u0442 \u043F\u043E\u0433\u043E\u0432\u043E\u0440\u0438\u0442\u044C",
    tag: "dad-wants-to-talk",
    requireInteraction: true,
    url: "/",
    actions: ["answer"]
  });
}

function notifyHelpRequest() {
  return sendPushToAll({
    type: "help_request",
    title: "\uD83C\uDD98 SOS \u0441\u0438\u0433\u043D\u0430\u043B!",
    body: "\u041D\u0435\u043C\u0435\u0434\u043B\u0435\u043D\u043D\u043E \u0441\u0432\u044F\u0436\u0438\u0442\u0435\u0441\u044C.",
    tag: "help-request",
    requireInteraction: true,
    url: "/",
    actions: ["answer"]
  });
}

function cleanupClient(ws) {
  const removedSender = senders.delete(ws);
  const removedReceiver = receivers.delete(ws);
  if (ws === phoneB) phoneB = receivers.values().next().value || null;
  if (removedSender || removedReceiver) setTimeout(broadcastStatus, 0);
}

function clientRole(ws) {
  return ws?.role || "unregistered";
}

function clientLabel(ws) {
  return `${clientRole(ws)}#${ws?.clientId || "?"}`;
}

function connectionCounts() {
  return `senders=${senders.size}, receivers=${receivers.size}, total=${wss.clients.size}`;
}

function formatCloseReason(reason) {
  const text = reason && reason.length ? reason.toString() : "";
  return text ? `, reason=${text}` : "";
}

function markClientAlive(ws) {
  if (!ws) return;
  ws.isAlive = true;
  ws.lastSeen = Date.now();
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
      console.log(`Send failed (${clientLabel(ws)}), terminating connection: ${error.message}`);
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

function sendToReceivers(data, excludeWs) {
  let delivered = 0;
  receivers.forEach((receiver) => {
    if (receiver !== excludeWs && safeSend(receiver, data)) delivered += 1;
  });
  return delivered;
}

function hasReceivers() {
  return receivers.size > 0;
}

wss.on("connection", (ws, req) => {
  ws.clientId = ++clientSeq;
  ws.role = "unregistered";
  markClientAlive(ws);
  ws.requestHost = req.headers.host;
  ws.requestProto = (req.headers["x-forwarded-proto"] || (req.socket.encrypted ? "https" : "http")).split(",")[0].trim();
  ws.requestIp = (req.headers["x-forwarded-for"] || req.socket.remoteAddress || "").split(",")[0].trim();
  console.log(`Client connected (${clientLabel(ws)}, ip=${ws.requestIp || "unknown"}, ${connectionCounts()})`);

  ws.on("pong", () => {
    markClientAlive(ws);
  });

  ws.on("message", (message) => {
    markClientAlive(ws);
    let data;
    try {
      data = JSON.parse(message);
    } catch {
      return;
    }

    if (data.type === "register" && data.role === "sender") {
      senders.add(ws);
      ws.role = "sender";
      console.log(`Sender registered (${clientLabel(ws)}, ${connectionCounts()})`);
      broadcastStatus();
    }
    if (data.type === "register" && data.role === "receiver") {
      receivers.add(ws);
      phoneB = ws;
      ws.role = "receiver";
      console.log(`Receiver registered (${clientLabel(ws)}, ${connectionCounts()})`);
      broadcastStatus();
    }
    if (data.type === "register" && data.role === "scheduler") {
      ws.role = "scheduler";
      console.log(`Scheduler registered (${clientLabel(ws)}, ${connectionCounts()})`);
    }
    if (data.type === "keepalive") {
      return;
    }
    if (data.type === "audio_message" && hasReceivers()) {
      const text = data.text || data.transcript || "";
      const audioFiles = saveAudioFiles(data, ws);
      console.log("audio_message -> receiver as voice_message", text ? `(${text.length} chars)` : "(no text)", audioFiles.audio_url || audioFiles.pcm_url || "(no audio url)");
      sendToReceivers({
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
      sendToReceivers(data);
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
    if (data.type === "direct_line_request" && hasReceivers()) {
      console.log("direct_line_request -> receiver");
      sendToReceivers({ type: "direct_line_request" });
    }
    if (data.type === "direct_line_cancel" && hasReceivers()) {
      console.log("direct_line_cancel -> receiver");
      sendToReceivers({ type: "direct_line_cancel" });
    }
    if (data.type === "direct_line_accept" && senders.size) {
      console.log(`direct_line_accept -> ${senders.size} sender(s)`);
      broadcastToSenders({ type: "direct_line_accept" });
    }
    if (data.type === "direct_line_timeout" && senders.size) {
      console.log(`direct_line_timeout -> ${senders.size} sender(s)`);
      broadcastToSenders({ type: "direct_line_timeout" });
    }
    if (data.type === "profile_sync" && hasReceivers()) {
      console.log("profile_sync -> receiver");
      sendToReceivers(data);
    }
    if (data.type === "reply_message" && senders.size) {
      console.log(`reply_message -> ${senders.size} sender(s)`);
      broadcastToSenders(data);
    }
    if (data.type === "dad_wants_to_talk") {
      console.log(`dad_wants_to_talk -> ${senders.size} sender(s), push=${pushSubscriptions.size}`);
      if (senders.size) broadcastToSenders({ type: "dad_wants_to_talk" });
      notifyDadWantsToTalk();
    }
    if (data.type === "wants_to_talk") {
      console.log(`wants_to_talk -> ${senders.size} sender(s), push=${pushSubscriptions.size}`);
      if (senders.size) broadcastToSenders({ type: "dad_wants_to_talk" });
      notifyDadWantsToTalk();
    }
    if (data.type === "help_request") {
      console.log(`help_request -> ${senders.size} sender(s), push=${pushSubscriptions.size}`);
      if (senders.size) broadcastToSenders({ type: "help_request" });
      notifyHelpRequest();
    }
    if (data.type === "sos_acknowledged" && hasReceivers()) {
      console.log("sos_acknowledged -> receiver");
      sendToReceivers({ type: "sos_acknowledged" });
    }
    if (data.type === "lunara_start" && senders.size) {
      console.log(`lunara_start -> ${senders.size} sender(s)`);
      broadcastToSenders({ type: "lunara_start" });
    }
    /* PATCH:liveLineHandlers */
    if (data.type === 'live_line_start') {
      if (hasReceivers()) {
        liveLineState.active = true;
        startLiveLineTimer();
        sendToReceivers({ type: 'live_line_start' });
        broadcastToSenders({ type: 'live_line_started' });
        console.log('live_line_start -> receiver');
      }
    }
    if (data.type === 'live_audio_chunk') {
      if (!liveLineState.active) return;
      if (ws.role === 'sender' && hasReceivers()) { sendToReceivers(data); }
      else if (ws.role === 'receiver') { broadcastToSenders(data); }
    }
    /* PATCH:liveWarning */
    if (data.type === 'live_line_warning') {
      if (ws.role === 'sender') sendToReceivers({type:'live_line_warning'});
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
    if (data.type === 'remind_later_ack' && hasReceivers()) {
      sendToReceivers({ type: 'remind_later_ack', minutes: data.minutes });
    }
  });

  ws.on("error", (error) => {
    console.log(`WebSocket error (${clientLabel(ws)}, ${connectionCounts()}): ${error.message}`);
    cleanupClient(ws);
  });

  ws.on("close", (code, reason) => {
    const label = clientLabel(ws);
    cleanupClient(ws);
    console.log(`Client disconnected (${label}, code=${code}${formatCloseReason(reason)}, ${connectionCounts()})`);
  });
});

const heartbeat = setInterval(() => {
  const now = Date.now();
  wss.clients.forEach((ws) => {
    const lastSeen = ws.lastSeen || now;
    if (now - lastSeen > HEARTBEAT_TIMEOUT_MS) {
      console.log(`Terminating stale client (${clientLabel(ws)}, last seen ${Math.round((now - lastSeen) / 1000)}s ago, ${connectionCounts()})`);
      cleanupClient(ws);
      ws.terminate();
      return;
    }

    ws.isAlive = false;
    try {
      ws.ping();
    } catch (error) {
      console.log(`WebSocket ping failed (${clientLabel(ws)}): ${error.message}`);
      cleanupClient(ws);
      ws.terminate();
    }
  });
}, HEARTBEAT_INTERVAL_MS);

wss.on("close", () => {
  clearInterval(heartbeat);
});

server.keepAliveTimeout = 65000;   /* keep Railway connection alive */
server.headersTimeout   = 66000;

server.listen(PORT, () => {
  console.log(`VoiceBridge test server running on http://localhost:${PORT}`);
});
