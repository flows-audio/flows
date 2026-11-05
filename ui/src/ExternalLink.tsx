import type { ReactNode } from 'react';
import { hostCapabilities, request } from './client';

/** A web link that opens outside the app on every host. */
export function ExternalLink({ href, children }: { href: string; children: ReactNode }) {
  return <a href={href} target="_blank" rel="noopener noreferrer" onClick={event => {
    // The native WebView cannot open tabs itself; the host opens the browser.
    if (hostCapabilities()?.externalLinks !== 'native') return;
    event.preventDefault();
    void request('browser.open', { url: href }).catch(() => {});
  }}>{children}</a>;
}
