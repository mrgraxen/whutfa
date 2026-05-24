import net from "node:net";
import fs from "node:fs";

export type SocketHandler = (data: Buffer) => void;

export function connectUnixSocket(
  path: string,
  onData: SocketHandler,
  onConnect?: () => void,
  onClose?: () => void,
): () => void {
  let socket: net.Socket | null = null;
  let buffer = Buffer.alloc(0);
  let stopped = false;
  let retryMs = 500;

  const connect = () => {
    if (stopped) return;
    if (process.platform === "win32") {
      console.warn(`[unix-socket] ${path}: Unix sockets not available on Windows; use AA_STUB=1`);
      setTimeout(connect, 2000);
      return;
    }
    if (!fs.existsSync(path)) {
      setTimeout(connect, retryMs);
      retryMs = Math.min(retryMs * 1.5, 5000);
      return;
    }
    retryMs = 500;
    socket = net.createConnection(path);
    socket.on("connect", () => {
      retryMs = 500;
      onConnect?.();
    });
    socket.on("data", (chunk) => {
      buffer = Buffer.concat([buffer, chunk]);
      onData(buffer);
      buffer = Buffer.alloc(0);
    });
    socket.on("error", () => {
      socket?.destroy();
      socket = null;
      onClose?.();
      setTimeout(connect, retryMs);
    });
    socket.on("close", () => {
      socket = null;
      onClose?.();
      setTimeout(connect, retryMs);
    });
  };

  connect();

  return () => {
    stopped = true;
    socket?.destroy();
  };
}

export function createUnixListener(
  path: string,
  onConnection: (socket: net.Socket) => void,
): Promise<net.Server> {
  return new Promise((resolve, reject) => {
    if (process.platform === "win32") {
      reject(new Error("Unix listen not supported on Windows"));
      return;
    }
    try {
      if (fs.existsSync(path)) fs.unlinkSync(path);
    } catch {
      /* ignore */
    }
    const server = net.createServer(onConnection);
    server.on("error", reject);
    server.listen(path, () => resolve(server));
  });
}

export function writeJsonLine(socket: net.Socket, obj: object): void {
  socket.write(JSON.stringify(obj) + "\n");
}
