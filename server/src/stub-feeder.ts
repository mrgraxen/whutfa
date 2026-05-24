import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import type { AppConfig } from "./config.js";
import { createUnixListener, writeJsonLine } from "./unix-socket.js";
import type net from "node:net";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

function readFixture(name: string): Buffer {
  const root = path.resolve(__dirname, "../../scripts/fixtures");
  const p = path.join(root, name);
  if (!fs.existsSync(p)) return Buffer.alloc(0);
  return fs.readFileSync(p);
}

/** When AA_STUB=1 and no aa-handler, emulate IPC on the Node side for dev/CI */
export async function startStubFeeder(config: AppConfig): Promise<() => void> {
  const h264 = readFixture("sample.h264");
  const pcm = readFixture("sample-media.pcm");
  const sockets: net.Socket[] = [];
  const cleanups: Array<() => void> = [];

  const bridgeServer = await createUnixListener(config.ipc.bridge_socket, (sock) => {
    sockets.push(sock);
    writeJsonLine(sock, { event: "session", state: "connecting" });
    setTimeout(() => {
      writeJsonLine(sock, {
        event: "video_config",
        width: config.video.width,
        height: config.video.height,
        fps: config.video.fps,
      });
      writeJsonLine(sock, { event: "session", state: "active" });
    }, 100);
  });
  cleanups.push(() => bridgeServer.close());

  const videoServer = await createUnixListener(config.ipc.video_socket, (sock) => {
    let ts = 0n;
    let offset = 0;
    const interval = setInterval(() => {
      if (h264.length === 0) return;
      const chunkSize = Math.min(4096, h264.length - offset);
      const chunk = h264.subarray(offset, offset + chunkSize);
      offset = (offset + chunkSize) % h264.length;
      const header = Buffer.alloc(12);
      const payloadLen = 8 + chunk.length;
      header.writeUInt32BE(payloadLen, 0);
      header.writeBigUInt64BE(ts, 4);
      ts += 33_000n;
      sock.write(Buffer.concat([header, chunk]));
    }, 33);
    sock.on("close", () => clearInterval(interval));
  });
  cleanups.push(() => videoServer.close());

  const audioServer = await createUnixListener(config.ipc.audio_media_socket, (sock) => {
    let ts = 0n;
    const interval = setInterval(() => {
      if (pcm.length === 0) return;
      const frame = pcm.subarray(0, Math.min(1920, pcm.length));
      const header = Buffer.alloc(20);
      header.writeUInt8(1, 0);
      header.writeUInt32BE(config.audio.media_sample_rate, 1);
      header.writeUInt8(2, 5);
      header.writeUInt16BE(16, 6);
      header.writeBigUInt64BE(ts, 8);
      header.writeUInt32BE(frame.length, 16);
      ts += 20_000n;
      sock.write(Buffer.concat([header, frame]));
    }, 20);
    sock.on("close", () => clearInterval(interval));
  });
  cleanups.push(() => audioServer.close());

  console.log("[stub-feeder] IPC sockets listening (AA_STUB server-side)");
  return () => {
    for (const s of sockets) s.destroy();
    for (const c of cleanups) c();
  };
}
