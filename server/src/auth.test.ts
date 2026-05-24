import { describe, it } from "node:test";
import assert from "node:assert";
import { validateToken, tokenFromUrl } from "./auth.js";

describe("auth", () => {
  it("validates token with timing-safe compare", () => {
    assert.equal(validateToken(undefined, "x"), true);
    assert.equal(validateToken("secret", "secret"), true);
    assert.equal(validateToken("secret", "wrong"), false);
  });

  it("reads token from url", () => {
    assert.equal(tokenFromUrl("/ws/video?token=abc"), "abc");
  });
});
