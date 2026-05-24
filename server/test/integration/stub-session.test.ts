import { describe, it, before, after } from "node:test";
import assert from "node:assert";
import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";
import http from "node:http";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(__dirname, "../../..");

describe("stub integration", () => {
  let proc: ReturnType<typeof spawn> | null = null;

  before(async () => {
    process.env.AA_STUB = "1";
    process.env.CONFIG_PATH = path.join(root, "config/config.yaml");
    process.env.HTTP_PORT = "18080";
    proc = spawn("node", ["dist/index.js"], {
      cwd: path.join(root, "server"),
      env: { ...process.env },
      stdio: "ignore",
    });
    await new Promise<void>((resolve, reject) => {
      const t = setTimeout(() => reject(new Error("timeout")), 8000);
      const check = () => {
        http
          .get("http://127.0.0.1:18080/healthz", (res) => {
            if (res.statusCode === 200) {
              clearTimeout(t);
              resolve();
            } else setTimeout(check, 200);
          })
          .on("error", () => setTimeout(check, 200));
      };
      setTimeout(check, 500);
    });
  });

  after(() => {
    proc?.kill();
  });

  it("returns status with stub flag on unix", async () => {
    if (process.platform === "win32") return;
    const body = await new Promise<string>((resolve, reject) => {
      http
        .get("http://127.0.0.1:18080/api/status", (res) => {
          let d = "";
          res.on("data", (c) => (d += c));
          res.on("end", () => resolve(d));
        })
        .on("error", reject);
    });
    const j = JSON.parse(body);
    assert.equal(j.stub, true);
  });
});
