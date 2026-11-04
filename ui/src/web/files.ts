// Browser file access for the web host: pickers, downloads and audio
// conversion. The core reads WAV and AIFF itself; other formats are decoded
// by the browser and handed over as float WAV.

const coreAudio = /\.(wav|aif|aiff|aifc)$/i;
export const browserAudio = /\.(flac|mp3|ogg|oga|opus|m4a|aac|webm)$/i;
export const audioAccept = '.wav,.aif,.aiff,.aifc,.flac,.mp3,.ogg,.oga,.opus,.m4a,.aac,.webm,audio/*';

/** Opens the browser file picker. Call synchronously from a click handler. */
export function pickFile(accept: string): Promise<File | null> {
  return new Promise(resolve => {
    const input = document.createElement('input');
    input.type = 'file'; input.accept = accept; input.style.display = 'none';
    let settled = false;
    const finish = (file: File | null) => { if (settled) return; settled = true; input.remove(); resolve(file); };
    input.addEventListener('change', () => finish(input.files?.[0] ?? null));
    input.addEventListener('cancel', () => finish(null));
    document.body.append(input);
    input.click();
  });
}

export function download(name: string, data: BlobPart, type = 'application/octet-stream') {
  const url = URL.createObjectURL(new Blob([data], { type }));
  const link = document.createElement('a');
  link.href = url; link.download = name; link.style.display = 'none';
  document.body.append(link); link.click(); link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
}

export function bytesToBase64(bytes: Uint8Array): string {
  let binary = '';
  for (let i = 0; i < bytes.length; i += 0x8000) binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(binary);
}

/** Float32 WAV, mono or stereo (extra channels are dropped). */
export function encodeWav(channels: Float32Array[], sampleRate: number): Uint8Array {
  const count = Math.min(2, channels.length), frames = channels[0]?.length ?? 0;
  const dataBytes = frames * count * 4, buffer = new ArrayBuffer(44 + dataBytes), view = new DataView(buffer);
  const text = (at: number, value: string) => { for (let i = 0; i < value.length; ++i) view.setUint8(at + i, value.charCodeAt(i)); };
  text(0, 'RIFF'); view.setUint32(4, 36 + dataBytes, true); text(8, 'WAVEfmt ');
  view.setUint32(16, 16, true); view.setUint16(20, 3, true); view.setUint16(22, count, true);
  view.setUint32(24, sampleRate, true); view.setUint32(28, sampleRate * count * 4, true);
  view.setUint16(32, count * 4, true); view.setUint16(34, 32, true); text(36, 'data'); view.setUint32(40, dataBytes, true);
  let at = 44;
  for (let i = 0; i < frames; ++i) for (let c = 0; c < count; ++c, at += 4) view.setFloat32(at, channels[c][i], true);
  return new Uint8Array(buffer);
}

/** Converts compressed audio to WAV for import; WAV/AIFF pass through untouched. */
export async function importableAudio(name: string, bytes: Uint8Array): Promise<{ name: string; bytes: Uint8Array }> {
  if (coreAudio.test(name) || !browserAudio.test(name)) return { name, bytes };
  const Offline = globalThis.OfflineAudioContext;
  if (!Offline) throw new Error('This browser cannot decode compressed audio. Import WAV or AIFF instead.');
  // decodeAudioData resamples to the context rate; 48 kHz keeps lossy sources intact enough.
  const context = new Offline({ numberOfChannels: 2, length: 1, sampleRate: 48000 });
  let decoded: AudioBuffer;
  try { decoded = await context.decodeAudioData(bytes.slice().buffer); }
  catch { throw new Error(`The browser could not decode ${name}.`); }
  const channels = Array.from({ length: Math.min(2, decoded.numberOfChannels) }, (_, c) => decoded.getChannelData(c));
  return { name: name.replace(/\.[^.]+$/, '') + '.wav', bytes: encodeWav(channels, decoded.sampleRate) };
}
