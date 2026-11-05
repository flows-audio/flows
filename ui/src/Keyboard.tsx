import { useCallback, useEffect, useRef, useState } from 'react';
import { Icon } from './Icon';
import { request } from './client';

const whiteSemitones = [0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23];
const blackSemitones = [1, 3, 6, 8, 10, 13, 15, 18, 20, 22];
const typingKeys = ['a', 'w', 's', 'e', 'd', 'f', 't', 'g', 'y', 'h', 'u', 'j', 'k', 'o', 'l', 'p', ';', "'"];
let nextNote = 1000000;
export function Keyboard({ documentId, connected, typing, visible, onError }: { documentId: string; connected: boolean; typing: boolean; visible: boolean; onError: (error: unknown) => void }) {
  const [octave, setOctave] = useState(2), [pressed, setPressed] = useState<Set<number>>(new Set());
  const octaveRef = useRef(octave);
  const held = useRef(new Map<string, { id: number; pitch: number; documentId: string }>());
  const ownedKeys = useRef(new Set<string>());
  const octaveKeys = useRef(new Set<string>());
  const noteOn = useCallback((key: string, pitch: number) => {
    if (!connected || held.current.has(key)) return;
    const id = ++nextNote; held.current.set(key, { id, pitch, documentId }); setPressed(new Set([...held.current.values()].map(n => n.pitch)));
    request('keyboard.noteOn', { noteId: id, pitch, velocity: .8, documentId }).catch(onError);
  }, [documentId, connected, onError]);
  const noteOff = useCallback((key: string) => {
    const note = held.current.get(key); if (!note) return;
    held.current.delete(key); setPressed(new Set([...held.current.values()].map(n => n.pitch)));
    request('keyboard.noteOff', { noteId: note.id, documentId: note.documentId }).catch(onError);
  }, [onError]);
  const release = useCallback(() => { [...held.current.keys()].forEach(noteOff); }, [noteOff]);
  const shiftOctave = useCallback((direction: number) => {
    const next = Math.max(0, Math.min(6, octaveRef.current + direction));
    if (next === octaveRef.current) return;
    release();
    octaveRef.current = next;
    setOctave(next);
  }, [release]);
  // Switching instruments releases audio while physical key ownership remains
  // until keyup, preventing held-key repeats from becoming source text.
  useEffect(() => { release(); }, [documentId, release]);
  useEffect(() => { if (!typing) release(); }, [typing, release]);
  useEffect(() => {
    const blur = () => { ownedKeys.current.clear(); octaveKeys.current.clear(); release(); };
    window.addEventListener('blur', blur); return () => { window.removeEventListener('blur', blur); blur(); };
  }, [release]);
  useEffect(() => {
    const down = (event: KeyboardEvent) => {
      const key = `key-${event.code}`;
      const target = event.target as HTMLElement;
      const eligible = typing && !event.isComposing && !event.metaKey && !event.ctrlKey && !event.altKey
        && !target.closest('input,textarea,select,[contenteditable=true],[role=listbox],[role=option],[role=menu],.step-sequencer');
      const direction = event.key.toLowerCase() === 'z' ? -1 : event.key.toLowerCase() === 'x' ? 1 : 0;
      if (octaveKeys.current.has(key)) {
        if (event.repeat) {
          event.preventDefault(); event.stopPropagation();
          if (eligible && connected && direction) shiftOctave(direction);
          return;
        }
        octaveKeys.current.delete(key);
      }
      // macOS repeats keydown while a key is held. Consume the owned key even
      // after focus/modifiers change, but send noteOn only on the first press.
      if (ownedKeys.current.has(key)) {
        if (event.repeat) { event.preventDefault(); event.stopPropagation(); return; }
        // A fresh press recovers if a previous keyup was lost.
        ownedKeys.current.delete(key); noteOff(key);
      }
      if (!eligible) return;
      if (direction) {
        event.preventDefault(); event.stopPropagation();
        octaveKeys.current.add(key);
        if (connected) shiftOctave(direction);
        return;
      }
      const index = typingKeys.indexOf(event.key.toLowerCase()); if (index < 0) return;
      event.preventDefault(); event.stopPropagation();
      if (!event.repeat) { ownedKeys.current.add(key); noteOn(key, (octaveRef.current + 1) * 12 + index); }
    };
    const up = (event: KeyboardEvent) => {
      const key = `key-${event.code}`;
      if (octaveKeys.current.delete(key)) { event.preventDefault(); event.stopPropagation(); return; }
      if (!ownedKeys.current.has(key)) return;
      ownedKeys.current.delete(key);
      event.preventDefault(); event.stopPropagation(); noteOff(key);
    };
    // Capture owned performance keys before an editor or native button handles
    // them. Unowned text-entry and shortcut events pass through unchanged.
    window.addEventListener('keydown', down, true); window.addEventListener('keyup', up, true);
    // Eligibility can briefly change while a knob edit compiles. Rebinding
    // listeners must not release a valid held note during that edit.
    return () => { window.removeEventListener('keydown', down, true); window.removeEventListener('keyup', up, true); };
  }, [typing, connected, shiftOctave, noteOn, noteOff, release]);
  useEffect(() => {
    // Collapsing the piano only removes its pointer targets. Physical keys stay
    // active, including ownership of repeats and the eventual keyup event.
    if (!visible) [...held.current.keys()].filter(key => key.startsWith('pointer-')).forEach(noteOff);
  }, [visible, noteOff]);
  if (!visible) return null;
  return <section className="keyboard-strip" id="keyboard-strip"><div className="keyboard-controls"><span className="eyebrow"><Icon name="keyboard" size={14}/> PLAY</span><div className="octave"><button title="Lower octave · Z in Perform mode" disabled={octave <= 0} onClick={() => shiftOctave(-1)}>−</button><span>C{octave}—B{octave + 1}</span><button title="Higher octave · X in Perform mode" disabled={octave >= 6} onClick={() => shiftOctave(1)}>+</button></div></div><div className="piano" aria-label="Two octave performance keyboard">
    {whiteSemitones.map((semitone, index) => { const pitch = (octave + 1) * 12 + semitone; return <button key={pitch} disabled={!connected} className={`piano-key white ${pressed.has(pitch) ? 'pressed' : ''}`} style={{ left: `${index / 14 * 100}%`, width: `${100 / 14}%` }} aria-label={`MIDI note ${pitch}`} onPointerDown={event => { event.currentTarget.setPointerCapture(event.pointerId); noteOn(`pointer-${event.pointerId}`, pitch); }} onPointerUp={event => noteOff(`pointer-${event.pointerId}`)} onPointerCancel={event => noteOff(`pointer-${event.pointerId}`)}>{semitone % 12 === 0 ? `C${octave + Math.floor(semitone / 12)}` : ''}</button>; })}
    {blackSemitones.map(semitone => { const pitch = (octave + 1) * 12 + semitone; const whitesBefore = whiteSemitones.filter(n => n < semitone).length; return <button key={pitch} disabled={!connected} className={`piano-key black ${pressed.has(pitch) ? 'pressed' : ''}`} style={{ left: `calc(${whitesBefore / 14 * 100}% - 2.1%)`, width: '4.2%' }} aria-label={`MIDI note ${pitch}`} onPointerDown={event => { event.currentTarget.setPointerCapture(event.pointerId); noteOn(`pointer-${event.pointerId}`, pitch); }} onPointerUp={event => noteOff(`pointer-${event.pointerId}`)} onPointerCancel={event => noteOff(`pointer-${event.pointerId}`)}/>; })}
  </div><button className="panic" onClick={() => { release(); request('keyboard.panic', { documentId }).catch(onError); }} disabled={!connected} title="Release all notes">PANIC</button></section>;
}
