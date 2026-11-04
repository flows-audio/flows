import { useEffect, useRef, useState } from 'react';
import { AgentEvent, Snapshot } from './types';
import { request, subscribe } from './client';
import { Icon } from './Icon';
import { Markdown } from './Markdown';
import { AssistantMessage, emptyHistory, HistoryFolder, HistoryView, historyRows, isTerminal, mergeAgentEvent, mergeLoadedMessages } from './assistantHistory';
import { AgentActivityView } from './AgentActivity';
import './Agent.css';

export function Agent({ snapshot, connected, flush, onResult, onError }: { snapshot: Snapshot | null; connected: boolean; flush: () => Promise<void>; onResult: (result: any) => void; onError: (error: unknown) => void }) {
  const [configuration, setConfiguration] = useState(false), [endpoint, setEndpoint] = useState('https://api.openai.com/v1'), [model, setModel] = useState(''), [apiKey, setApiKey] = useState('');
  const sessionKeys = snapshot?.capabilities?.secrets === 'session';
  const [profileId, setProfileId] = useState(''), [profileName, setProfileName] = useState(''), [savingModel, setSavingModel] = useState(false);
  const [history, setHistory] = useState<HistoryView>(emptyHistory), [historyOpen, setHistoryOpen] = useState(false), [folder, setFolder] = useState<HistoryFolder>('active'), [query, setQuery] = useState('');
  const [reference, setReference] = useState('');
  const [prompt, setPrompt] = useState(''), [review, setReview] = useState<string | null>(null), [error, setError] = useState(''), [loading, setLoading] = useState(false);
  const [renameId, setRenameId] = useState(''), [renameTitle, setRenameTitle] = useState(''), [purgeId, setPurgeId] = useState('');
  const [localRunning, setLocalRunning] = useState<{ requestId: string; conversationId: string } | null>(null);
  const historyRef = useRef(history), localRef = useRef(localRunning), epoch = useRef(0), cache = useRef(new Map<string, AssistantMessage[]>());
  const loadPending = useRef(false), localPhase = useRef<'preparing' | 'sent'>('preparing'), locallyCancelled = useRef(new Set<string>());
  const end = useRef<HTMLDivElement>(null), mounted = useRef(true), loadRef = useRef<(method?: string, params?: Record<string, unknown>) => Promise<HistoryView | undefined>>();
  const commit = (view: HistoryView) => { historyRef.current = view; setHistory(view); };
  const markRunning = (value: typeof localRunning) => { localRef.current = value; setLocalRunning(value); };
  const report = (value: unknown) => setError(value instanceof Error ? value.message : String(value));
  const load = async (method = 'ai.conversations.list', params: Record<string, unknown> = {}) => {
    const ticket = ++epoch.current; loadPending.current = true; setLoading(true);
    try {
      const result = await request<HistoryView>(method, params);
      if (!mounted.current || ticket !== epoch.current) return undefined;
      if (result.conversation) {
        const conversation = result.conversation;
        const messages = mergeLoadedMessages(conversation.messages, cache.current.get(conversation.id) || []);
        cache.current.set(conversation.id, messages);
        result.conversation = { ...conversation, messages };
      }
      commit(result); setError(''); return result;
    } catch (value) { if (mounted.current && ticket === epoch.current) report(value); return undefined; }
    finally { if (mounted.current && ticket === epoch.current) { loadPending.current = false; setLoading(false); } }
  };
  loadRef.current = load;
  useEffect(() => { mounted.current = true; if (connected) void loadRef.current?.(); return () => { mounted.current = false; ++epoch.current; }; }, [connected, snapshot?.workspaceId]);
  const profiles = snapshot?.ai.profiles || [];
  const editModel = (id: string) => {
    const profile = profiles.find(p => p.id === id);
    setProfileId(profile?.id || ''); setProfileName(profile?.name || '');
    setEndpoint(profile?.endpoint || 'https://api.openai.com/v1'); setModel(profile?.model || ''); setApiKey('');
  };
  useEffect(() => { if (!configuration) editModel(snapshot?.ai.selectedId || ''); }, [snapshot?.ai.selectedId, snapshot?.ai.endpoint, snapshot?.ai.model, configuration]);
  const changeModel = async (method: string, params: Record<string, unknown>) => {
    setSavingModel(true);
    try { const result = await request(method, params); onResult(result); setError(''); return true; }
    catch (value) { report(value); return false; }
    finally { setSavingModel(false); }
  };
  useEffect(() => subscribe<AgentEvent>('flowsAgent', event => {
    if (!event?.requestId || !event.conversationId) return;
    const conversationId = event.conversationId;
    const messages = mergeAgentEvent(cache.current.get(conversationId) || [], event);
    cache.current.set(conversationId, messages);
    const current = historyRef.current;
    if (current.conversation?.id === conversationId) commit({ ...current, conversation: { ...current.conversation, messages } });
    if (isTerminal(event.status || '')) {
      if (localRef.current?.requestId === event.requestId) markRunning(null);
      // Refresh saved summaries and the currently selected conversation. An
      // epoch guard prevents an earlier request from reversing a later switch.
      if (!loadPending.current) void loadRef.current?.();
    }
  }), []);
  const conversation = history.conversation, messages = conversation?.messages || [];
  const busy = !!localRunning || !!snapshot?.ai.busy;
  const canAsk = connected && !!snapshot?.ai.configured;
  const runningId = localRunning?.conversationId || snapshot?.ai.runningConversationId || '';
  const runningRequest = localRunning?.requestId || snapshot?.ai.runningRequestId || '';
  const readonly = !!conversation && (conversation.archived || conversation.deleted);
  useEffect(() => { if (!historyOpen && !configuration) end.current?.scrollIntoView({ block: 'end' }); }, [conversation?.id, messages.length, messages[messages.length - 1]?.text, messages[messages.length - 1]?.activity, historyOpen, configuration]);
  const select = async (id: string) => {
    setReview(null); setPrompt(''); setRenameId(''); setPurgeId('');
    const result = await load('ai.conversations.select', { conversationId: id });
    if (result) { setHistoryOpen(false); setConfiguration(false); }
  };
  const mutate = async (action: string, id: string, title?: string) => {
    const result = await load('ai.conversations.' + action, { conversationId: id, ...(title === undefined ? {} : { title }) });
    if (result) { setRenameId(''); setPurgeId(''); setReview(null); }
  };
  const create = async () => {
    const result = await load('ai.conversations.create');
    if (result) { setHistoryOpen(false); setConfiguration(false); setPrompt(''); setReview(null); }
  };
  const send = async () => {
    if (!prompt.trim() || busy || loading || readonly || !connected || !snapshot?.ai.configured) return;
    const text = prompt.trim();
    let conversationId = conversation?.id || '';
    const requestId = 'agent-' + Date.now() + '-' + Math.random().toString(36).slice(2, 8);
    localPhase.current = 'preparing'; markRunning({ requestId, conversationId }); setPrompt(''); setError('');
    try {
      if (!conversationId) { const created = await load('ai.conversations.create'); if (!created?.conversation) throw new Error('Could not create a conversation.'); conversationId = created.conversation.id; }
      if (locallyCancelled.current.has(requestId)) return;
      markRunning({ requestId, conversationId });
      await flush();
      if (locallyCancelled.current.has(requestId)) return;

      const message: AssistantMessage = { id: requestId, prompt: text, text: '', status: 'working', startedAt: Date.now(), baseRevision: 0, documentId: reference };
      const currentMessages = [...(cache.current.get(conversationId) || []), message]; cache.current.set(conversationId, currentMessages);
      const current = historyRef.current;
      if (current.conversation?.id === conversationId) commit({ ...current, conversation: { ...current.conversation, messages: currentMessages } });
      localPhase.current = 'sent';
      const acknowledgement = await request('ai.send', { requestId, conversationId, prompt: text, referencePath: reference });
      // Completion may already have arrived by this point.
      const merged = mergeAgentEvent(cache.current.get(conversationId) || [], acknowledgement);
      cache.current.set(conversationId, merged);
      const latest = historyRef.current;
      if (latest.conversation?.id === conversationId) commit({ ...latest, conversation: { ...latest.conversation, messages: merged } });
    } catch (value) {
      if (locallyCancelled.current.has(requestId)) return;
      const failure = { requestId, conversationId, status: 'failed', error: value instanceof Error ? value.message : String(value) };
      const merged = mergeAgentEvent(cache.current.get(conversationId) || [], failure); cache.current.set(conversationId, merged);
      const current = historyRef.current;
      if (current.conversation?.id === conversationId) commit({ ...current, conversation: { ...current.conversation, messages: merged } });
      markRunning(null); report(value); setPrompt(text);
    } finally { locallyCancelled.current.delete(requestId); }
  };
  const cancel = async () => {
    if (localRef.current && localPhase.current === 'preparing') { locallyCancelled.current.add(localRef.current.requestId); markRunning(null); return; }
    try { await request('ai.cancel', { requestId: runningRequest }); } catch (value) { report(value); }
  };
  const rows = historyRows(history.conversations, folder, query);
  return <aside className="agent-panel">
    <header className="panel-heading"><span><Icon name="spark"/> ASSISTANT</span><div className="agent-header-actions">
      <button className="icon-button" title="New conversation" aria-label="New conversation" disabled={!connected || loading} onClick={() => void create()}><Icon name="add"/></button>
      <button className={'icon-button ' + (historyOpen ? 'selected' : '')} title="Conversation history" aria-label="Conversation history" aria-expanded={historyOpen} onClick={() => { setHistoryOpen(!historyOpen); setConfiguration(false); setReview(null); if (!historyOpen) void load(); }}><Icon name="history"/></button>
      <button className={'icon-button ' + (configuration ? 'selected' : '')} title="AI connection settings" aria-label="AI connection settings" onClick={() => { setConfiguration(!configuration); setHistoryOpen(false); }}><Icon name="settings"/></button>
    </div></header>
    {configuration ? <form className="ai-configuration" onSubmit={async event => { event.preventDefault(); if (await changeModel('ai.models.save', { ...(profileId ? { id: profileId } : {}), name: profileName, endpoint, model, ...(apiKey ? { apiKey } : {}) })) { setApiKey(''); setConfiguration(false); } }}>
      <div className="section-title">Global models</div><p>Available in every conversation and instrument. {sessionKeys ? 'API keys stay in this browser tab only and are forgotten when it closes. Your endpoint must allow requests from this page (CORS).' : 'API keys are saved in macOS Keychain.'}</p>
      {(error || snapshot?.ai.settingsError) && <div className="agent-history-error" role="alert">{error || snapshot?.ai.settingsError}</div>}
      <label>Saved models<select aria-label="Saved models" value={profileId} disabled={savingModel || busy} onChange={event => editModel(event.target.value)}><option value="">New model…</option>{profiles.map(p => <option key={p.id} value={p.id}>{p.name}</option>)}</select></label>
      <label>Name<input aria-label="Model display name" value={profileName} onChange={event => setProfileName(event.target.value)} placeholder="Optional display name" maxLength={100}/></label>
      <label>Endpoint<input value={endpoint} onChange={event => setEndpoint(event.target.value)} placeholder="https://…/v1" required/></label>
      <label>Model<input value={model} onChange={event => setModel(event.target.value)} placeholder="Model identifier" maxLength={200} required/></label>
      <label>API key<input type="password" value={apiKey} onChange={event => setApiKey(event.target.value)} placeholder={profiles.find(p => p.id === profileId)?.hasKey ? `${sessionKeys ? 'Kept for this tab' : 'Saved in Keychain'} · leave blank to keep` : 'API key'} autoComplete="off"/></label>
      <div className="model-settings-actions"><button className="primary-button" disabled={!connected || busy || savingModel}>{savingModel ? 'Saving…' : 'Save model'}</button>{profileId && <button type="button" className="secondary-button" disabled={busy || savingModel} onClick={async () => { if (await changeModel('ai.models.remove', { id: profileId })) editModel(''); }}>Remove model</button>}</div>
    </form> : <>
      <div className="agent-context"><span className={'connection-dot ' + (snapshot?.ai.configured ? 'active' : '')}/><span>{snapshot?.ai.configured ? snapshot.ai.model || 'Connected model' : 'No model connected'}</span><span className="spacer"/>{snapshot?.ai.configured && <small>LIVE</small>}</div>
      {(error || snapshot?.ai.settingsError || history.historyError || snapshot?.ai.historyError) && <div className="agent-history-error" role="alert">{error || snapshot?.ai.settingsError || history.historyError || snapshot?.ai.historyError}</div>}
      {historyOpen ? <div className="agent-history">
        <div className="agent-history-tabs" role="tablist" aria-label="Conversation folders">{(['active', 'archived', 'trash'] as HistoryFolder[]).map(item => <button key={item} role="tab" aria-selected={folder === item} className={folder === item ? 'selected' : ''} onClick={() => { setFolder(item); setRenameId(''); setPurgeId(''); }}>{item === 'active' ? 'Active' : item === 'archived' ? 'Archived' : 'Trash'}</button>)}</div>
        <input className="agent-history-search" aria-label="Search conversations" placeholder="Search conversations…" value={query} onChange={event => setQuery(event.target.value)}/>
        <div className="agent-history-list">{!rows.length && <p className="agent-history-empty">{loading ? 'Loading conversations…' : 'No conversations here.'}</p>}
          {rows.map(row => <div className={'agent-history-row ' + (row.id === conversation?.id ? 'selected' : '')} data-conversation-id={row.id} key={row.id}>
            {renameId === row.id ? <form className="agent-history-rename" onSubmit={event => { event.preventDefault(); void mutate('rename', row.id, renameTitle); }}>
              <input aria-label="Conversation title" autoFocus maxLength={120} value={renameTitle} onChange={event => setRenameTitle(event.target.value)} onKeyDown={event => { if (event.key === 'Escape') setRenameId(''); }}/><button type="submit" disabled={loading || !renameTitle.trim()}>Save</button><button type="button" onClick={() => setRenameId('')}>Cancel</button>
            </form> : <button className="agent-history-open" title={'Open ' + row.title} disabled={loading} onClick={() => void select(row.id)}><strong>{row.title}</strong><small>{row.messageCount} {row.messageCount === 1 ? 'message' : 'messages'} · {new Date(row.updatedAt).toLocaleDateString('en-US', { month: 'short', day: 'numeric' })}{runningId === row.id ? ' · Working…' : ''}</small></button>}
            <div className="agent-history-actions">
              <button title="Rename conversation" disabled={loading} onClick={() => { setRenameId(row.id); setRenameTitle(row.title); setPurgeId(''); }}>Rename</button>
              {row.archived || row.deleted ? <button title="Restore conversation" disabled={loading} onClick={() => void mutate('restore', row.id)}>Restore</button> : <button title="Archive conversation" disabled={loading || runningId === row.id} onClick={() => void mutate('archive', row.id)}>Archive</button>}
              {row.deleted ? <button className="danger" title="Delete permanently" disabled={loading || runningId === row.id} onClick={() => setPurgeId(row.id)}>Delete permanently</button> : <button title="Move conversation to Trash" disabled={loading || runningId === row.id} onClick={() => void mutate('delete', row.id)}>Move to Trash</button>}
            </div>
            {purgeId === row.id && <div className="agent-history-confirm">Delete this conversation permanently? This cannot be undone.<div><button className="danger" disabled={loading} onClick={() => void mutate('purge', row.id)}>Delete forever</button><button onClick={() => setPurgeId('')}>Cancel</button></div></div>}
          </div>)}
        </div><div className="agent-history-bottom"><span>Saved in this workspace</span><button disabled={loading} onClick={() => void load()}>Refresh</button></div>
      </div> : <>
        <div className="agent-conversation-heading"><strong>{conversation?.title || 'New conversation'}</strong><small>{conversation?.deleted ? 'TRASH' : conversation?.archived ? 'ARCHIVED' : ''}</small></div>
        <div className="agent-messages">{!messages.length && <div className="agent-empty"><div className="spark-orbit"><Icon name="spark" size={27}/></div><h3>A second pair of ears.<br/>A clearer line of code.</h3><p>Ask about a signal, shape a sound, or examine what changed. Every proposed edit stays yours to review.</p>{!snapshot?.ai.configured && <button className="secondary-button" onClick={() => setConfiguration(true)} disabled={!connected}>Connect a model <Icon name="chevron" size={13}/></button>}<div className="prompt-suggestions">{['Explain this signal flow', 'Help me shape this bass', 'Compare before and after'].map(text => <button key={text} disabled={!canAsk || readonly} onClick={() => setPrompt(text)}>{text}<Icon name="add" size={13}/></button>)}</div></div>}
          {messages.map(message => <div className="agent-exchange" key={message.id}><div className="user-message">{message.prompt}</div><div className={'assistant-message ' + (message.status === 'failed' ? 'failed' : '')}><div className="assistant-label"><Icon name="spark" size={13}/> FLOWS <span>{message.status === 'working' ? 'Working' : message.status === 'done' ? 'Complete' : message.status}</span></div><AgentActivityView message={message}/>{message.text && <div className="assistant-text"><Markdown text={message.text}/></div>}
            {message.proposedSource && <><button className="secondary-button" onClick={() => setReview(review === message.id ? null : message.id)}>{review === message.id ? 'Hide proposed source' : 'Review proposed source'}<Icon name="code" size={14}/></button>{review === message.id && <><p className="proposal-target">Target: {message.documentId || 'New instrument in AI-gen'}</p><pre className="source-review">{message.proposedSource}</pre>{message.documentId && message.documentId !== snapshot?.document.id && <button className="secondary-button" disabled={busy || readonly} onClick={async () => { try { await flush(); onResult(await request('document.open', { path: message.documentId })); } catch (value) { report(value); } }}>Open {message.documentId} to review</button>}{message.documentId && message.documentId === snapshot?.document.id && <button className="primary-button" disabled={busy || readonly || !message.proposalAvailable} onClick={async () => { try { await flush(); onResult(await request('ai.apply', { requestId: message.id, documentId: message.documentId, baseRevision: message.baseRevision, text: message.proposedSource })); setReview(null); await load(); } catch (value) { report(value); } }}>Apply source edit</button>}<button className="primary-button" disabled={busy || readonly || !message.proposalAvailable} onClick={async () => { try { await flush(); onResult(await request('ai.create', { requestId: message.id, documentId: message.documentId, baseRevision: message.baseRevision, text: message.proposedSource })); setReview(null); await load(); } catch (value) { report(value); } }}>Save new instrument in AI-gen</button>{!message.proposalAvailable && <div className="proposal-unavailable">This saved proposal is available to read. Ask for a new proposal to apply changes.</div>}</>}</>}
          </div></div>)}<div ref={end}/>
        </div>
        {readonly ? <div className="agent-readonly">This conversation is {conversation?.deleted ? 'in Trash' : 'archived'}.<button disabled={loading} onClick={() => void mutate('restore', conversation!.id)}>Restore to continue</button></div> : <div className="agent-composer"><textarea aria-label="Assistant prompt" value={prompt} onChange={event => setPrompt(event.target.value)} onKeyDown={event => { if (event.key === 'Enter' && (event.metaKey || event.ctrlKey)) { event.preventDefault(); void send(); } }} placeholder={!snapshot?.ai.configured ? 'Connect a model to begin…' : 'Ask about your workspace or describe an instrument…'} disabled={!canAsk || busy || loading} rows={3}/><div className="composer-footer"><label className="agent-reference"><Icon name="file" size={12}/><select aria-label="Instrument reference" value={reference} disabled={busy} onChange={event => setReference(event.target.value)}><option value="">Workspace · no reference</option>{snapshot?.files.filter(file => file.kind === 'instrument').map(file => <option key={file.path} value={file.path}>{file.path}</option>)}</select></label>{busy ? <button className="send-button" title="Cancel AI request" disabled={!runningRequest} onClick={() => void cancel()}><Icon name="stop" size={14}/></button> : <button className="send-button" title="Send · ⌘ Enter" disabled={!canAsk || !prompt.trim() || loading} onClick={() => void send()}><Icon name="arrow" size={17}/></button>}</div></div>}
        <div className="agent-model-picker"><label><span>Model</span><select aria-label="Assistant model" value={snapshot?.ai.selectedId || ''} disabled={!connected || busy || savingModel || !profiles.length} onChange={event => void changeModel('ai.models.select', { id: event.target.value })}>{!profiles.length && <option value="">No saved models</option>}{profiles.map(p => <option key={p.id} value={p.id}>{p.name}{!p.hasKey && !/^http:\/\/(localhost|127\.0\.0\.1)(:|\/|$)/.test(p.endpoint) ? ' · Add key' : ''}</option>)}</select></label><button className="icon-button" title="Manage global models" aria-label="Manage global models" disabled={!connected || busy} onClick={() => { editModel(snapshot?.ai.selectedId || ''); setConfiguration(true); }}><Icon name="settings" size={14}/></button></div>
        {busy && runningId && runningId !== conversation?.id && <div className="agent-running">A reply is being prepared in another conversation.<button onClick={() => void select(runningId)}>View</button>{readonly && <button disabled={!runningRequest} onClick={() => void cancel()}>Cancel request</button>}</div>}
        <div className="agent-disclaimer">Source edits require your review.</div>
      </>}
    </>}
  </aside>;
}
