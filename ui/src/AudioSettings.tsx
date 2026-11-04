// In-app Audio & MIDI settings for hosts without a native device dialog
// (capabilities.audioSettings === 'in-app', the browser).
import { useEffect, useState } from 'react';
import { request, subscribe } from './client';
import { Icon } from './Icon';
import type { AudioDevices } from './types';

function formatBytes(value?: number) {
  if (value === undefined) return '';
  const units = ['bytes', 'KB', 'MB', 'GB'];
  let index = 0, amount = value;
  while (amount >= 1024 && index < units.length - 1) { amount /= 1024; ++index; }
  return `${amount < 10 && index ? amount.toFixed(1) : Math.round(amount)} ${units[index]}`;
}
type Storage = { persisted: boolean; usage?: number; quota?: number };

export function AudioSettings({ onClose }: { onClose: () => void }) {
  const [devices, setDevices] = useState<AudioDevices>();
  const [storage, setStorage] = useState<Storage>();
  const [error, setError] = useState('');
  useEffect(() => {
    void request('audio.devices').then(setDevices).catch(value => setError(value instanceof Error ? value.message : String(value)));
    return subscribe('flowsDevices', setDevices);
  }, []);
  useEffect(() => {
    const manager = navigator.storage;
    void Promise.all([manager?.persisted?.() ?? false, manager?.estimate?.()])
      .then(([persisted, estimate]) => setStorage({ persisted, usage: estimate?.usage, quota: estimate?.quota }))
      .catch(() => {});
  }, []);
  const act = (method: 'audio.start' | 'audio.suspend' | 'midi.request', params = {}) =>
    void request(method, params).then(setDevices).then(() => setError('')).catch(value => setError(value instanceof Error ? value.message : String(value)));
  const toggle = (id: string, enabled: boolean) =>
    void request('midi.enable', { id, enabled }).then(setDevices).catch(value => setError(String(value)));
  const keep = () => void navigator.storage?.persist?.().then(persisted => setStorage(current => ({ ...(current ?? {}), persisted })));

  const audio = devices?.audio, midi = devices?.midi;
  const live = audio?.state === 'running';
  const latency = audio?.outputLatency || audio?.baseLatency;
  return <div className="appearance-popover audio-settings" role="dialog" aria-label="Audio and MIDI">
    <header><span>Audio &amp; MIDI</span><button className="icon-button" title="Close audio and MIDI settings" onClick={onClose}><Icon name="close" size={14}/></button></header>
    {error && <div className="audio-settings-error" role="alert">{error}</div>}
    <section aria-labelledby="audio-output-heading">
      <h3 id="audio-output-heading"><Icon name="sound" size={14}/>Output</h3>
      {audio?.state === 'unsupported' ? <p>This browser cannot run the Flows audio engine. Use a current Chrome, Edge, Firefox or Safari.</p> : <>
        <div className="device-row"><span className={`connection-dot ${live ? 'active' : ''}`}/>
          <strong>{live ? 'Sound on' : audio?.state === 'starting' ? 'Starting…' : audio?.state === 'failed' ? 'Sound failed' : 'Sound off'}</strong>
          {live && audio && <small>{(audio.sampleRate / 1000).toFixed(1)} kHz{latency ? ` · ${Math.round(latency * 1000)} ms` : ''}</small>}
        </div>
        {audio?.error && <p className="audio-settings-detail">{audio.error}</p>}
        {!live && <p>Browsers start sound after you press a key or click. Editing and compiling keep working while sound is off.</p>}
        {live ? <button className="secondary-button" onClick={() => act('audio.suspend')}>Pause sound</button>
          : <button className="primary-button" disabled={audio?.state === 'starting'} onClick={() => act('audio.start')}>Start sound</button>}
      </>}
    </section>
    <section aria-labelledby="midi-heading">
      <h3 id="midi-heading"><Icon name="keyboard" size={14}/>MIDI keyboards</h3>
      {midi?.state === 'unsupported' && <p>This browser has no Web MIDI. Play the on-screen keys or your computer keyboard (A W S E D F T G Y H U J K). Chrome, Edge and Firefox support MIDI keyboards.</p>}
      {midi?.state === 'prompt' && <><p>Allow MIDI to play Flows from a connected keyboard. Notes work in Edit and Perform modes.</p><button className="secondary-button" onClick={() => act('midi.request')}>Allow MIDI keyboards</button></>}
      {midi?.state === 'denied' && <p>MIDI is blocked for this site. Allow it in the browser’s site settings, then reload the page.</p>}
      {midi?.state === 'granted' && (midi.inputs.length
        ? <ul className="midi-inputs">{midi.inputs.map(input => <li key={input.id}>
            <label><input type="checkbox" checked={input.enabled} onChange={event => toggle(input.id, event.target.checked)}/>
              <span>{input.name}</span>{!input.connected && <small>Disconnected</small>}</label></li>)}</ul>
        : <p>No MIDI keyboards connected. Plug one in and it turns on automatically.</p>)}
    </section>
    <section aria-labelledby="storage-heading">
      <h3 id="storage-heading"><Icon name="folder" size={14}/>Storage</h3>
      <p>Your workspace is saved in this browser on this device{storage?.persisted ? '' : ', and the browser may clear it when space runs low'}. Export instruments to keep copies elsewhere.</p>
      <div className="device-row">{storage?.usage !== undefined && <small>{formatBytes(storage.usage)} used</small>}
        {storage && !storage.persisted && <button className="text-button" onClick={keep}>Keep this workspace</button>}</div>
    </section>
  </div>;
}
