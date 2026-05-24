/** Parse video frame: [uint32_be len][uint64_be ts_us][h264] — len includes ts+h264 payload after length field semantics from plan */

export interface VideoFrame {
  timestampUs: bigint;
  data: Buffer;
  keyframe: boolean;
}

/** Payload after outer length prefix: [uint64_be ts_us][h264 bytes] */
export function parseVideoPayload(payload: Buffer): VideoFrame | null {
  if (payload.length < 8) return null;
  const timestampUs = payload.readBigUInt64BE(0);
  const data = payload.subarray(8);
  const keyframe = detectKeyframe(data);
  return { timestampUs, data, keyframe };
}

export function packVideoForWs(frame: VideoFrame): Buffer {
  const prefix = Buffer.alloc(1);
  prefix[0] = frame.keyframe ? 1 : 0;
  return Buffer.concat([prefix, frame.data]);
}

export interface AudioFrame {
  type: number;
  sampleRate: number;
  channels: number;
  bitsPerSample: number;
  timestampUs: bigint;
  pcm: Buffer;
}

export function parseAudioPacket(buf: Buffer): AudioFrame | null {
  if (buf.length < 19) return null;
  const type = buf.readUInt8(0);
  const sampleRate = buf.readUInt32BE(1);
  const channels = buf.readUInt8(5);
  const bitsPerSample = buf.readUInt16BE(6);
  const timestampUs = buf.readBigUInt64BE(8);
  const pcmLen = buf.readUInt32BE(16);
  if (buf.length < 20 + pcmLen) return null;
  const pcm = buf.subarray(20, 20 + pcmLen);
  return { type, sampleRate, channels, bitsPerSample, timestampUs, pcm };
}

export function packAudioForWs(frame: AudioFrame): Buffer {
  const header = Buffer.alloc(17);
  header.writeUInt8(frame.type, 0);
  header.writeUInt32BE(frame.sampleRate, 1);
  header.writeUInt8(frame.channels, 5);
  header.writeBigUInt64BE(frame.timestampUs, 6);
  return Buffer.concat([header, frame.pcm]);
}

function detectKeyframe(h264: Buffer): boolean {
  let i = 0;
  while (i < h264.length - 4) {
    let start = -1;
    if (h264[i] === 0 && h264[i + 1] === 0 && h264[i + 2] === 1) start = i + 3;
    else if (h264[i] === 0 && h264[i + 1] === 0 && h264[i + 2] === 0 && h264[i + 3] === 1)
      start = i + 4;
    if (start >= 0 && start < h264.length) {
      const nalType = h264[start] & 0x1f;
      if (nalType === 5 || nalType === 7 || nalType === 8) return true;
    }
    i++;
  }
  return false;
}
