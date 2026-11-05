import type { SequencerPattern } from './protocol.gen';
export type { SequencerLeafSound, SequencerPattern, SequencerRow, SequencerSound, SequencerState, SequencerStep } from './protocol.gen';
import type { SequencerLeafSound } from './protocol.gen';

const SCALES = {
  major: [0, 2, 4, 5, 7, 9, 11], minor: [0, 2, 3, 5, 7, 8, 10], pentatonic: [0, 2, 4, 7, 9],
};
// Display the same pitch emitted by StepSequencer.hpp, including downward ties
// and scale roots that wrap across the MIDI endpoints.
export function sequenceNote(note: number, pattern: Pick<SequencerPattern, 'transpose' | 'scale' | 'root'>) {
  const pitch = Math.max(0, Math.min(127, note + pattern.transpose));
  if (pattern.scale === 'chromatic') return pitch;
  for (let distance = 0; distance < 12; ++distance) for (const candidate of [pitch - distance, pitch + distance]) {
    if (candidate >= 0 && candidate <= 127 && SCALES[pattern.scale].includes((candidate - pattern.root + 132) % 12)) return candidate;
  }
  return pitch;
}

export function withLaneSound(pattern: SequencerPattern, index: number, sound: SequencerLeafSound): SequencerPattern {
  const lanes = pattern.rows.map((_, i): SequencerLeafSound => pattern.sound?.kind === 'lanes' ? pattern.sound.lanes[i] ?? { kind: 'active' } : pattern.sound ?? { kind: 'active' });
  return { ...pattern, sound: { kind: 'lanes', lanes: lanes.map((value, i) => i === index ? sound : value) },
    rows: pattern.rows.map((value, i) => i === index && sound.kind === 'sample' && (lanes[i].kind !== 'sample' || (lanes[i] as { path: string }).path !== sound.path) ? { ...value, note: 48 } : value) };
}
