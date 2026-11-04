// Browser-only notices: where work is saved (first visit) and a narrow-window
// warning. Each is dismissible and remembered.
import { useEffect, useState } from 'react';
import { Icon } from '../Icon';

const welcomeKey = 'flows.web.welcomed';
const minimumWidth = 760;
function remembered(key: string) { try { return localStorage.getItem(key) === '1'; } catch { return false; } }
function remember(key: string) { try { localStorage.setItem(key, '1'); } catch { /* session only */ } }

export function WebNotices({ onExport }: { onExport: () => void }) {
  const [welcome, setWelcome] = useState(() => !remembered(welcomeKey));
  const [narrow, setNarrow] = useState(() => window.innerWidth < minimumWidth);
  const [continued, setContinued] = useState(false);
  useEffect(() => {
    const resize = () => setNarrow(window.innerWidth < minimumWidth);
    window.addEventListener('resize', resize);
    return () => window.removeEventListener('resize', resize);
  }, []);
  const dismiss = () => { remember(welcomeKey); setWelcome(false); };
  return <>
    {welcome && <div className="web-welcome" role="status">
      <Icon name="folder" size={15}/>
      <span>Your instruments are saved in this browser, not on a server. Export an instrument package to keep a copy or move it to another computer.</span>
      <button className="secondary-button" onClick={() => { onExport(); dismiss(); }}><Icon name="export" size={14}/>Export instrument</button>
      <button className="icon-button" title="Dismiss" aria-label="Dismiss" onClick={dismiss}><Icon name="close" size={14}/></button>
    </div>}
    {narrow && !continued && <div className="modal-backdrop" role="presentation">
      <div className="rename-dialog close-dialog narrow-notice" role="alertdialog" aria-labelledby="narrow-title" aria-describedby="narrow-text">
        <h2 id="narrow-title">Flows needs a wider window</h2>
        <p id="narrow-text">The editor, signal views and keyboard need at least {minimumWidth} pixels. Widen the window, turn the device sideways, or open Flows on a computer.</p>
        <div className="dialog-actions"><button className="secondary-button" onClick={() => setContinued(true)}>Continue anyway</button></div>
      </div>
    </div>}
  </>;
}
