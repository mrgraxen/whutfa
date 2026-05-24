export class LengthPrefixedReader {
  private buffer = Buffer.alloc(0);

  push(chunk: Buffer, onFrame: (frame: Buffer) => void): void {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (this.buffer.length >= 4) {
      const len = this.buffer.readUInt32BE(0);
      if (len > 10_000_000) {
        this.buffer = Buffer.alloc(0);
        break;
      }
      if (this.buffer.length < 4 + len) break;
      const frame = this.buffer.subarray(4, 4 + len);
      this.buffer = this.buffer.subarray(4 + len);
      onFrame(frame);
    }
  }
}
