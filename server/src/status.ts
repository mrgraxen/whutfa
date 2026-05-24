export interface HandlerStatus {
  usb_phase: string;
  aoap_open: boolean;
  ssl_ok: boolean;
  session_active: boolean;
  last_error: string | null;
  dropped_frames: number;
}

export const defaultStatus: HandlerStatus = {
  usb_phase: "idle",
  aoap_open: false,
  ssl_ok: false,
  session_active: false,
  last_error: null,
  dropped_frames: 0,
};

export function applyControlEvent(status: HandlerStatus, msg: Record<string, unknown>): HandlerStatus {
  const next = { ...status };
  const event = msg.event as string | undefined;
  if (event === "session") {
    const state = msg.state as string;
    next.session_active = state === "active";
    if (state === "connecting") next.usb_phase = "aoa_switch";
    if (state === "active") {
      next.aoap_open = true;
      next.ssl_ok = true;
      next.usb_phase = "session_active";
    }
    if (state === "stopped") {
      next.session_active = false;
      next.usb_phase = "idle";
    }
  }
  if (event === "error") {
    next.last_error = `${msg.code}: ${msg.hint ?? ""}`;
    const code = String(msg.code ?? "");
    if (code.startsWith("AOA")) next.usb_phase = "aoa_failed";
    if (code.startsWith("SSL")) {
      next.ssl_ok = false;
      next.usb_phase = "ssl_failed";
    }
  }
  if (event === "stats" && typeof msg.dropped_frames === "number") {
    next.dropped_frames = msg.dropped_frames;
  }
  return next;
}
