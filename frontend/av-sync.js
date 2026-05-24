/** Schedule audio relative to video timestamps (microseconds) */

export class AvSync {
  constructor(targetBufferMs = 80) {
    this.targetBufferMs = targetBufferMs;
    this.originVideoTs = null;
    this.originPerf = null;
    this.pending = [];
  }

  onFirstVideoTs(timestampUs) {
    if (this.originVideoTs !== null) return;
    this.originVideoTs = Number(timestampUs);
    this.originPerf = performance.now();
  }

  /** Returns delay in ms before playing this audio chunk */
  scheduleAudio(timestampUs) {
    if (this.originVideoTs === null || this.originPerf === null) return 0;
    const targetMs = (Number(timestampUs) - this.originVideoTs) / 1000;
    const elapsed = performance.now() - this.originPerf;
    return Math.max(0, targetMs - elapsed);
  }

  get bufferMs() {
    return this.pending.length * 20;
  }
}
