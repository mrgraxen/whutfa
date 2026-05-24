import { describe, it } from "node:test";
import assert from "node:assert";
import { AvSync } from "./av-sync.js";

describe("av-sync", () => {
  it("schedules after video origin", () => {
    const sync = new AvSync();
    sync.onFirstVideoTs(1_000_000n);
    const d = sync.scheduleAudio(1_020_000n);
    assert.ok(d >= 0);
  });
});
