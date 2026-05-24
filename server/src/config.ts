import fs from "node:fs";
import path from "node:path";
import yaml from "yaml";

export interface AppConfig {
  head_unit: {
    manufacturer: string;
    model: string;
    name: string;
  };
  video: { width: number; height: number; fps: number; dpi: number };
  audio: { media_sample_rate: number; speech_sample_rate: number };
  server: {
    http_port: number;
    bind_address: string;
    access_token: string;
    bind_localhost_only: boolean;
  };
  security: { require_token: boolean };
  bluetooth: { enabled: boolean; adapter_name: string; pin: string };
  sensors: {
    night_mode: boolean;
    driving_status: string;
    gps: { enabled: boolean; latitude: number; longitude: number };
  };
  diagnostics: { log_level: string; ssl_clock_max_skew_sec: number };
  ipc: {
    bridge_socket: string;
    video_socket: string;
    audio_media_socket: string;
    audio_speech_socket: string;
    audio_system_socket: string;
  };
}

const defaults: AppConfig = {
  head_unit: { manufacturer: "PiHeadUnit", model: "WebHU-1", name: "Pi Head Unit" },
  video: { width: 1280, height: 720, fps: 30, dpi: 160 },
  audio: { media_sample_rate: 48000, speech_sample_rate: 16000 },
  server: {
    http_port: 8080,
    bind_address: "0.0.0.0",
    access_token: "",
    bind_localhost_only: false,
  },
  security: { require_token: false },
  bluetooth: { enabled: true, adapter_name: "PiHeadUnit", pin: "0000" },
  sensors: {
    night_mode: false,
    driving_status: "parked",
    gps: { enabled: false, latitude: 0, longitude: 0 },
  },
  diagnostics: { log_level: "info", ssl_clock_max_skew_sec: 300 },
  ipc: {
    bridge_socket: "/tmp/aa-bridge.sock",
    video_socket: "/tmp/aa-video.sock",
    audio_media_socket: "/tmp/aa-audio-media.sock",
    audio_speech_socket: "/tmp/aa-audio-speech.sock",
    audio_system_socket: "/tmp/aa-audio-system.sock",
  },
};

export function loadConfig(): AppConfig {
  const configPath = process.env.CONFIG_PATH ?? "/config/config.yaml";
  let merged: AppConfig = { ...defaults };
  if (fs.existsSync(configPath)) {
    const raw = yaml.parse(fs.readFileSync(configPath, "utf8")) as Partial<AppConfig>;
    merged = deepMerge(
      defaults as unknown as Record<string, unknown>,
      raw as unknown as Record<string, unknown>,
    ) as unknown as AppConfig;
  }
  if (process.env.HTTP_PORT) merged.server.http_port = Number(process.env.HTTP_PORT);
  if (process.env.BIND_ADDRESS) merged.server.bind_address = process.env.BIND_ADDRESS;
  if (process.env.ACCESS_TOKEN) merged.server.access_token = process.env.ACCESS_TOKEN;
  if (merged.server.access_token) merged.security.require_token = true;
  if (merged.server.bind_localhost_only) merged.server.bind_address = "127.0.0.1";
  return merged;
}

function deepMerge<T extends Record<string, unknown>>(base: T, patch: Partial<T>): T {
  const out = { ...base };
  for (const key of Object.keys(patch) as (keyof T)[]) {
    const v = patch[key];
    if (v && typeof v === "object" && !Array.isArray(v) && typeof base[key] === "object") {
      out[key] = deepMerge(base[key] as Record<string, unknown>, v as Record<string, unknown>) as T[keyof T];
    } else if (v !== undefined) {
      out[key] = v as T[keyof T];
    }
  }
  return out;
}

export function projectRoot(): string {
  return path.resolve(path.dirname(new URL(import.meta.url).pathname), "../..");
}
