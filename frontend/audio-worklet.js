class PcmPlayerProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = [];
    this.port.onmessage = (e) => {
      if (e.data.type === "pcm") {
        this.queue.push(new Int16Array(e.data.pcm));
      }
    };
  }

  process(inputs, outputs) {
    const output = outputs[0];
    if (!output?.length) return true;
    const ch0 = output[0];
    let i = 0;
    while (i < ch0.length) {
      if (this.queue.length === 0) {
        ch0.fill(0, i);
        break;
      }
      const chunk = this.queue[0];
      const take = Math.min(chunk.length, ch0.length - i);
      for (let j = 0; j < take; j++) {
        ch0[i + j] = chunk[j] / 32768;
      }
      i += take;
      if (take >= chunk.length) this.queue.shift();
      else this.queue[0] = chunk.subarray(take);
    }
    if (output[1]) {
      for (let k = 0; k < ch0.length; k++) output[1][k] = ch0[k];
    }
    return true;
  }
}

registerProcessor("pcm-player", PcmPlayerProcessor);
