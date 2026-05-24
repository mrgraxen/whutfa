import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { WebSocketServer, WebSocket } from "ws";
import { loadConfig } from "./config.js";
import { validateToken, tokenFromUrl } from "./auth.js";
import {
  parseVideoPayload,
  parseAudioPacket,
  packVideoForWs,
  packAudioForWs,
} from "./ipc-framing.js";
import { connectUnixSocket } from "./unix-socket.js";
import { LengthPrefixedReader } from "./stream-buffer.js";
import { BridgeClient } from "./bridge-client.js";
import { startStubFeeder } from "./stub-feeder.js";
import type { HandlerStatus } from "./status.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const frontendDir = path.resolve(__dirname, "../../frontend");

let handlerStatus: HandlerStatus = {
  usb_phase: "idle",
  aoap_open: false,
  ssl_ok: false,
  session_active: false,
  last_error: null,
  dropped_frames: 0,
};

let videoConfig = { width: 1280, height: 720, fps: 30 };

async function main(): Promise<void> {
  const config = loadConfig();
  const useStub = process.env.AA_STUB === "1";

  if (useStub && process.platform !== "win32") {
    await startStubFeeder(config);
  }

  const bridge = new BridgeClient(config.ipc.bridge_socket);
  bridge.setHandlers(
    (s) => {
      handlerStatus = s;
    },
    (vc) => {
      videoConfig = vc;
    },
  );
  bridge.start();

  let activeVideoWs: WebSocket | null = null;
  let activeAudioWs: WebSocket | null = null;

  const broadcastVideo = (data: Buffer) => {
    if (activeVideoWs?.readyState === WebSocket.OPEN) activeVideoWs.send(data);
  };

  const broadcastAudio = (data: Buffer) => {
    if (activeAudioWs?.readyState === WebSocket.OPEN) activeAudioWs.send(data);
  };

  if (process.platform !== "win32") {
    const videoReader = new LengthPrefixedReader();
    connectUnixSocket(
      config.ipc.video_socket,
      (buf) => {
        videoReader.push(buf, (payload) => {
          const frame = parseVideoPayload(payload);
          if (frame) broadcastVideo(packVideoForWs(frame));
        });
      },
      undefined,
      undefined,
    );

    const audioPaths = [
      { path: config.ipc.audio_media_socket, type: 1 },
      { path: config.ipc.audio_speech_socket, type: 2 },
    ];
    for (const { path: sockPath, type } of audioPaths) {
      connectUnixSocket(sockPath, (buf) => {
        const frame = parseAudioPacket(buf);
        if (frame) {
          frame.type = type;
          broadcastAudio(packAudioForWs(frame));
        }
      });
    }
  }

  const checkAuth = (url: string, headers: http.IncomingHttpHeaders): boolean => {
    if (!config.security.require_token) return true;
    const token =
      tokenFromUrl(url) ??
      (typeof headers["x-access-token"] === "string" ? headers["x-access-token"] : undefined);
    return validateToken(config.server.access_token, token);
  };

  const server = http.createServer((req, res) => {
    const url = req.url ?? "/";
    if (url.startsWith("/healthz")) {
      res.writeHead(200, { "Content-Type": "text/plain" });
      res.end("ok");
      return;
    }
    if (url.startsWith("/api/status")) {
      if (!checkAuth(url, req.headers)) {
        res.writeHead(401);
        res.end();
        return;
      }
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(
        JSON.stringify({
          ...handlerStatus,
          video: videoConfig,
          stub: useStub,
        }),
      );
      return;
    }
    if (!checkAuth(url, req.headers)) {
      res.writeHead(401, { "Content-Type": "text/html" });
      res.end("<h1>Unauthorized</h1><p>Set ?token= or X-Access-Token</p>");
      return;
    }
    serveStatic(req, res, url.split("?")[0]);
  });

  const wss = new WebSocketServer({ noServer: true });

  server.on("upgrade", (req, socket, head) => {
    const url = req.url ?? "/";
    if (!checkAuth(url, req.headers)) {
      socket.write("HTTP/1.1 401 Unauthorized\r\n\r\n");
      socket.destroy();
      return;
    }
    wss.handleUpgrade(req, socket, head, (ws) => {
      const pathname = url.split("?")[0];
      if (pathname === "/ws/video") {
        if (activeVideoWs) {
          ws.close(4000, JSON.stringify({ error: "session_in_use" }));
          return;
        }
        activeVideoWs = ws;
        ws.on("close", () => {
          if (activeVideoWs === ws) activeVideoWs = null;
        });
      } else if (pathname === "/ws/audio") {
        if (activeAudioWs) {
          ws.close(4000, JSON.stringify({ error: "session_in_use" }));
          return;
        }
        activeAudioWs = ws;
        ws.on("close", () => {
          if (activeAudioWs === ws) activeAudioWs = null;
        });
      } else if (pathname === "/ws/input") {
        ws.on("message", (raw) => {
          try {
            const msg = JSON.parse(String(raw)) as Record<string, unknown>;
            const x = Number(msg.x);
            const y = Number(msg.y);
            const ax = Math.round(x * videoConfig.width);
            const ay = Math.round(y * videoConfig.height);
            bridge.sendTouch({
              type: "touch",
              action: msg.action,
              x: ax / videoConfig.width,
              y: ay / videoConfig.height,
            });
          } catch {
            /* ignore */
          }
        });
      } else {
        ws.close();
      }
    });
  });

  const port = config.server.http_port;
  const host = config.server.bind_address;
  server.listen(port, host, () => {
    console.log(`[server] http://${host}:${port} stub=${useStub}`);
  });
}

function serveStatic(req: http.IncomingMessage, res: http.ServerResponse, pathname: string): void {
  let file = pathname === "/" ? "/index.html" : pathname;
  const fp = path.join(frontendDir, path.normalize(file).replace(/^(\.\.(\/|\\|$))+/, ""));
  if (!fp.startsWith(frontendDir) || !fs.existsSync(fp) || fs.statSync(fp).isDirectory()) {
    res.writeHead(404);
    res.end("Not found");
    return;
  }
  const ext = path.extname(fp);
  const types: Record<string, string> = {
    ".html": "text/html",
    ".js": "application/javascript",
    ".css": "text/css",
  };
  res.writeHead(200, { "Content-Type": types[ext] ?? "application/octet-stream" });
  fs.createReadStream(fp).pipe(res);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
