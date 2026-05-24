import net from "node:net";
import fs from "node:fs";
import { applyControlEvent, defaultStatus, type HandlerStatus } from "./status.js";

export class BridgeClient {
  status: HandlerStatus = { ...defaultStatus };
  private socket: net.Socket | null = null;
  private lineBuf = "";
  private onStatus?: (s: HandlerStatus) => void;
  private onVideoConfig?: (c: { width: number; height: number; fps: number }) => void;

  constructor(
    private path: string,
    private reconnect = true,
  ) {}

  setHandlers(
    onStatus: (s: HandlerStatus) => void,
    onVideoConfig?: (c: { width: number; height: number; fps: number }) => void,
  ): void {
    this.onStatus = onStatus;
    this.onVideoConfig = onVideoConfig;
  }

  start(): () => void {
    let stopped = false;
    let retry = 500;

    const connect = () => {
      if (stopped || process.platform === "win32") return;
      if (!fs.existsSync(this.path)) {
        setTimeout(connect, retry);
        retry = Math.min(retry * 1.5, 5000);
        return;
      }
      retry = 500;
      this.socket = net.createConnection(this.path);
      this.socket.setEncoding("utf8");
      this.socket.on("connect", () => console.log("[bridge] connected"));
      this.socket.on("data", (chunk: string) => {
        this.lineBuf += chunk;
        let idx: number;
        while ((idx = this.lineBuf.indexOf("\n")) >= 0) {
          const line = this.lineBuf.slice(0, idx).trim();
          this.lineBuf = this.lineBuf.slice(idx + 1);
          if (!line) continue;
          try {
            const msg = JSON.parse(line) as Record<string, unknown>;
            this.status = applyControlEvent(this.status, msg);
            this.onStatus?.(this.status);
            if (msg.event === "video_config") {
              this.onVideoConfig?.({
                width: Number(msg.width),
                height: Number(msg.height),
                fps: Number(msg.fps),
              });
            }
          } catch {
            /* ignore */
          }
        }
      });
      this.socket.on("close", () => {
        this.socket = null;
        if (this.reconnect && !stopped) setTimeout(connect, retry);
      });
      this.socket.on("error", () => this.socket?.destroy());
    };

    connect();
    return () => {
      stopped = true;
      this.socket?.destroy();
    };
  }

  sendTouch(payload: object): void {
    if (this.socket?.writable) {
      this.socket.write(JSON.stringify(payload) + "\n");
    }
  }
}
