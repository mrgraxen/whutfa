/** Annex-B H.264 utilities for WebCodecs */

export function splitAnnexB(buffer) {
  const units = [];
  let i = 0;
  const len = buffer.byteLength;
  const view = new Uint8Array(buffer);

  const findStart = (from) => {
    for (let j = from; j < len - 3; j++) {
      if (view[j] === 0 && view[j + 1] === 0) {
        if (view[j + 2] === 1) return j;
        if (view[j + 2] === 0 && view[j + 3] === 1) return j;
      }
    }
    return -1;
  };

  const startLen = (pos) => {
    if (view[pos + 2] === 1) return 3;
    return 4;
  };

  while (i < len) {
    const start = findStart(i);
    if (start < 0) break;
    const scLen = startLen(start);
    const nalStart = start + scLen;
    const next = findStart(nalStart);
    const nalEnd = next < 0 ? len : next;
    if (nalEnd > nalStart) {
      units.push(view.subarray(nalStart, nalEnd));
    }
    i = next < 0 ? len : next;
  }
  return units;
}

export function nalType(nal) {
  return nal[0] & 0x1f;
}

export function extractSpsPps(units) {
  let sps = null;
  let pps = null;
  for (const u of units) {
    const t = nalType(u);
    if (t === 7) sps = u;
    if (t === 8) pps = u;
  }
  return { sps, pps };
}

/** Build avcC description for VideoDecoder.configure */
export function buildAvcDescription(sps, pps) {
  if (!sps || !pps) return null;
  const enc = new TextEncoder();
  const profile = sps[1];
  const compatibility = sps[2];
  const level = sps[3];

  const spsLen = sps.byteLength;
  const ppsLen = pps.byteLength;
  const out = new Uint8Array(11 + spsLen + ppsLen);
  let o = 0;
  out[o++] = 1;
  out[o++] = profile;
  out[o++] = compatibility;
  out[o++] = level;
  out[o++] = 0xff;
  out[o++] = 0xe1;
  out[o++] = (spsLen >> 8) & 0xff;
  out[o++] = spsLen & 0xff;
  out.set(sps, o);
  o += spsLen;
  out[o++] = 1;
  out[o++] = (ppsLen >> 8) & 0xff;
  out[o++] = ppsLen & 0xff;
  out.set(pps, o);
  return out.buffer;
}

export function codecStringFromSps(sps) {
  if (!sps || sps.byteLength < 4) return "avc1.640028";
  const toHex = (n) => n.toString(16).padStart(2, "0");
  return `avc1.${toHex(sps[1])}${toHex(sps[2])}${toHex(sps[3])}`;
}

export function isKeyframeAccessUnit(units) {
  return units.some((u) => {
    const t = nalType(u);
    return t === 5 || t === 7 || t === 8;
  });
}

export function supportsWebCodecs() {
  return typeof VideoDecoder !== "undefined";
}
