// Screens shown before the editor: starting, unsupported browser, another tab
// owns the workspace, or startup failed.
import { ExternalLink } from '../ExternalLink';
import iconUrl from '../../../assets/flows-icon.svg?url';

export type BootKind = 'loading' | 'unsupported' | 'busy' | 'moved' | 'failed';

export function BootScreen({ kind, detail, onTakeOver }: { kind: BootKind; detail?: string; onTakeOver?: () => void }) {
  return <main className="boot-screen" aria-busy={kind === 'loading'}>
    <div className="boot-panel">
      <img className="boot-icon" src={iconUrl} alt="" width={56} height={56}/>
      {kind === 'loading' && <>
        <h1>Flows</h1>
        <p className="boot-status" role="status"><span className="boot-progress" aria-hidden="true"/>{detail ?? 'Starting the instrument engine…'}</p>
      </>}
      {kind === 'unsupported' && <>
        <h1>This browser can’t run Flows</h1>
        <p>{detail}</p>
        <p>Flows runs in current Chrome, Edge, Firefox and Safari. You can also <ExternalLink href="https://github.com/flows-audio/flows/releases/latest">download the macOS app</ExternalLink>.</p>
      </>}
      {kind === 'busy' && <>
        <h1>Flows is open in another tab</h1>
        <p>Your workspace opens in one tab at a time. Continuing here saves the other tab’s work and closes Flows there.</p>
        <div className="boot-actions"><button className="primary-button" onClick={onTakeOver}>Use Flows here</button></div>
      </>}
      {kind === 'moved' && <>
        <h1>Flows moved to another tab</h1>
        <p>Your work was saved. Reload to bring Flows back to this tab.</p>
        <div className="boot-actions"><button className="primary-button" onClick={() => location.reload()}>Reload here</button></div>
      </>}
      {kind === 'failed' && <>
        <h1>Flows could not start</h1>
        <p>{detail}</p>
        <div className="boot-actions"><button className="secondary-button" onClick={() => location.reload()}>Try again</button></div>
      </>}
    </div>
  </main>;
}
