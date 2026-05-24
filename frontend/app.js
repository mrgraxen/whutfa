import {
  splitAnnexB,
  extractSpsPps,
  buildAvcDescription,
  codecStringFromSps,
  isKeyframeAccessUnit,
  supportsWebCodecs,
} from "./h264.js";
import { AvSync } from "./av-sync.js";

const canvas = document.getElementById("screen");
const overlay = document.getElementById("overlay");
const wrap = document.getElementById("wrap");
const unsupported = document.getElementById("unsupported");

const params = new URLSearchParams(location.search);
const token = params.get("token");
const wsQuery = token ? `?token=${encodeURIComponent(token)}` : "";

let aaWidth = 1280;
let aaHeight = 720;
let fpsCount = 0;
let lastFpsTime = performance.now();
let pendingDecode = 0;
let decoder = null;
let avSync = new AvSync();
let audioCtx = null;
let workletNode = null;

function wsUrl(path) {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  return `${proto}//${location.host}${path}${wsQuery}`;
}

function setOverlay(lines) {
  overlay.innerHTML = lines.map((l) => `<div>${l}</div>`).join("");
}

async function pollStatus() {
  try {
    const url = token ? `/api/status?token=${encodeURIComponent(token)}` : "/api/status";
    const r = await fetch(url);
    const s = await r.json();
    if (s.video) {
      aaWidth = s.video.width;
      aaHeight = s.video.height;
      canvas.width = aaWidth;
      canvas.height = aaHeight;
    }
    const lines = [
      `USB: ${s.usb_phase}`,
      `Session: ${s.session_active ? "active" : "no"}`,
      `FPS: ${fpsCount}`,
      s.stub ? "<span class='warn'>STUB</span>" : "",
    ];
    if (s.last_error) lines.push(`<span class='warn'>${s.last_error}</span>`);
    setOverlay(lines);
  } catch {
    setOverlay(["Status unavailable"]);
  }
  setTimeout(pollStatus, 2000);
}

async function initDecoder() {
  decoder = new VideoDecoder({
    output: (frame) => {
      const ctx = canvas.getContext("2d");
      ctx.drawImage(frame, 0, 0, canvas.width, canvas.height);
      frame.close();
      fpsCount++;
      pendingDecode = Math.max(0, pendingDecode - 1);
    },
    error: (e) => console.error("VideoDecoder", e),
  });
}

async function configureDecoder(sps, pps) {
  const desc = buildAvcDescription(sps, pps);
  if (!desc) return;
  const codec = codecStringFromSps(sps);
  decoder.configure({
    codec,
    codedWidth: aaWidth,
    codedHeight: aaHeight,
    description: desc,
  });
}

function connectVideo() {
  const ws = new WebSocket(wsUrl("/ws/video"));
  ws.binaryType = "arraybuffer";
  ws.onmessage = async (ev) => {
    const buf = new Uint8Array(ev.data);
    const keyFlag = buf[0];
    const data = buf.subarray(1);
    const units = splitAnnexB(data.buffer);
    if (units.length === 0) return;

    const { sps, pps } = extractSpsPps(units);
    if (sps && pps && decoder.state === "unconfigured") {
      await configureDecoder(sps, pps);
    }

    if (pendingDecode > 2 && !isKeyframeAccessUnit(units) && keyFlag !== 1) return;

    const type = keyFlag === 1 || isKeyframeAccessUnit(units) ? "key" : "delta";
    if (decoder.state !== "configured") return;

    pendingDecode++;
    decoder.decode(
      new EncodedVideoChunk({
        type,
        timestamp: performance.now() * 1000,
        data,
      }),
    );
  };
  ws.onclose = (ev) => {
    if (ev.code === 4001 || ev.code === 4000) {
      setTimeout(connectVideo, 2000);
    }
  };
}

async function connectAudio() {
  audioCtx = new AudioContext({ sampleRate: 48000 });
  await audioCtx.audioWorklet.addModule("/audio-worklet.js");
  workletNode = new AudioWorkletNode(audioCtx, "pcm-player", {
    numberOfInputs: 0,
    numberOfOutputs: 1,
    outputChannelCount: [2],
  });
  workletNode.connect(audioCtx.destination);

  const ws = new WebSocket(wsUrl("/ws/audio"));
  ws.binaryType = "arraybuffer";
  ws.onmessage = (ev) => {
    const v = new DataView(ev.data);
    const sampleRate = v.getUint32(1);
    const channels = v.getUint8(5);
    const ts = v.getBigUint64(6);
    const pcm = ev.data.slice(17);
    avSync.onFirstVideoTs(ts);
    const delay = avSync.scheduleAudio(ts);
    setTimeout(() => {
      if (audioCtx?.state === "suspended") audioCtx.resume();
      workletNode.port.postMessage(
        { type: "pcm", pcm, sampleRate, channels },
        [pcm],
      );
    }, delay);
  };
}

function connectInput() {
  const ws = new WebSocket(wsUrl("/ws/input"));

  const send = (action, e) => {
    const rect = canvas.getBoundingClientRect();
    const x = (e.clientX - rect.left) / rect.width;
    const y = (e.clientY - rect.top) / rect.height;
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: "touch", action, x, y }));
    }
  };

  canvas.addEventListener("pointerdown", (e) => {
    canvas.setPointerCapture(e.pointerId);
    send("down", e);
  });
  canvas.addEventListener("pointermove", (e) => send("move", e));
  canvas.addEventListener("pointerup", (e) => send("up", e));
}

async function main() {
  if (!supportsWebCodecs()) {
    wrap.style.display = "none";
    unsupported.style.display = "block";
    return;
  }
  await initDecoder();
  connectVideo();
  connectAudio();
  connectInput();
  pollStatus();
  setInterval(() => {
    const now = performance.now();
    if (now - lastFpsTime >= 1000) {
      fpsCount = 0;
      lastFpsTime = now;
    }
  }, 1000);
}

main();
