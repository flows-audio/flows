import { Children, isValidElement, memo, useState, type ReactNode } from 'react';
import ReactMarkdown, { type Components } from 'react-markdown';
import remarkGfm from 'remark-gfm';
import { hostCapabilities, request } from './client';
import './Markdown.css';

// Assistant output is untrusted. Accept explicit web links, never host-resource
// URLs or schemes that can execute code or open local applications.
export function safeMarkdownUrl(value: string): string {
  if (!/^https?:\/\//i.test(value) || /[\u0000-\u0020\u007f\\]/.test(value)) return '';
  try {
    const url = new URL(value);
    return (url.protocol === 'https:' || url.protocol === 'http:') && !url.username && !url.password ? url.href : '';
  } catch { return ''; }
}

function MarkdownLink({ href, children, title }: { href?: string; children: ReactNode; title?: string }) {
  const [error, setError] = useState('');
  const safe = safeMarkdownUrl(href || '');
  if (!safe) return <span>{children}</span>;
  return <><a href={safe} target="_blank" rel="noopener noreferrer" title={title} onClick={event => {
    // Hosts that cannot open a browser tab themselves (the WebView) open links natively.
    if (hostCapabilities()?.externalLinks !== 'native') return;
    event.preventDefault(); setError('');
    void request('browser.open', { url: safe }).catch(() => setError('Could not open this link.'));
  }}>{children}</a>{error && <span className="markdown-action-error" role="status"> {error}</span>}</>;
}

async function copyCode(text: string): Promise<void> {
  if (hostCapabilities()?.clipboard === 'native') { await request('clipboard.write', { text }); return; }
  if (!navigator.clipboard?.writeText) throw new Error('Clipboard unavailable');
  await navigator.clipboard.writeText(text);
}

const flowsTokens = /(#.*$)|("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')|(\b(?:import|from|as|return|class|def|if|else|and|or|not)\b)|(\b(?:Inst|std|Classic\w+|self)\b)|(\b(?:\d+(?:\.\d*)?|\.\d+)(?:khz|hz|ms|db|s)?\b)|(\b[a-zA-Z_]\w*(?=\s*\())|(\b[a-zA-Z_]\w*(?=\s*:))|([+*\/=<>!:\-])/gm;
const tokenClasses = ['comment', 'string', 'keyword', 'type', 'number', 'function', 'property', 'operator'];
function highlightedFlows(text: string): ReactNode[] {
  const parts: ReactNode[] = []; let end = 0;
  for (const match of text.matchAll(flowsTokens)) {
    const start = match.index ?? 0;
    if (start > end) parts.push(text.slice(end, start));
    const kind = match.slice(1).findIndex(value => value !== undefined);
    parts.push(<span className={`markdown-token-${tokenClasses[kind]}`} key={start}>{match[0]}</span>);
    end = start + match[0].length;
  }
  if (end < text.length) parts.push(text.slice(end));
  return parts;
}

function CodeBlock({ children }: { children: ReactNode }) {
  const code = Children.toArray(children).find(isValidElement);
  const props = code?.props as { className?: string; children?: ReactNode } | undefined;
  const text = String(props?.children ?? '').replace(/\n$/, '');
  const language = props?.className?.match(/\blanguage-([^\s]+)/)?.[1] || 'code';
  const [copyState, setCopyState] = useState<'idle' | 'copying' | 'copied' | 'error'>('idle');
  return <div className="markdown-code-block">
    <div className="markdown-code-heading"><span>{language}</span><button type="button" title="Copy code" disabled={copyState === 'copying'} onClick={() => {
      setCopyState('copying');
      void copyCode(text).then(() => setCopyState('copied'), () => setCopyState('error'));
    }}>{copyState === 'copied' ? 'Copied' : copyState === 'copying' ? 'Copying…' : copyState === 'error' ? 'Retry copy' : 'Copy'}</button></div>
    <pre><code className={props?.className}>{language.toLowerCase() === 'flows' ? highlightedFlows(text) : text}</code></pre>
    {copyState === 'error' && <span className="markdown-action-error" role="status">Could not copy. Select the code and press ⌘ C.</span>}
    {copyState === 'copied' && <span className="markdown-copy-status" role="status">Code copied to clipboard.</span>}
  </div>;
}

const components: Components = {
  a: ({ href, children, title }) => <MarkdownLink href={href} title={title}>{children}</MarkdownLink>,
  // Render remote images as explicit links so replies cannot silently request
  // tracking pixels or leak the user's IP address to arbitrary image hosts.
  img: ({ src, alt }) => <span className="markdown-image"><MarkdownLink href={typeof src === 'string' ? src : undefined}>{alt ? `Image: ${alt}` : 'Image'}</MarkdownLink></span>,
  pre: ({ children }) => <CodeBlock>{children}</CodeBlock>,
  table: ({ children }) => <div className="markdown-table-scroll" tabIndex={0} role="region" aria-label="Markdown table"><table>{children}</table></div>,
};

export const Markdown = memo(function Markdown({ text }: { text: string }) {
  return <div className="markdown-body"><ReactMarkdown remarkPlugins={[remarkGfm]} components={components} skipHtml urlTransform={safeMarkdownUrl}>{text}</ReactMarkdown></div>;
});
