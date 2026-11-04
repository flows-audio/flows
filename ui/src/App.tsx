import type { Projection } from './types';
import { RefObject, useCallback, useEffect, useRef, useState } from 'react';
import { Editor } from './Editor';
import { Explorer } from './Explorer';
import { StepSequencer } from './StepSequencer';
import { Agent } from './Agent';
import { Keyboard } from './Keyboard';
import { Icon } from './Icon';
import { AudioReadout, TempoControl, SignalView } from './Signals';
import { hostAvailable, request, subscribe, transportKind } from './client';
import { AudioSettings } from './AudioSettings';
import { ExternalLink } from './ExternalLink';
import { editorShortcut, textSizeShortcut } from './keymap';
import { audioAccept, bytesToBase64, download, importableAudio, pickFile } from './web/files';
import { WebNotices } from './web/WebNotices';
import { publishTelemetry, resetTelemetry, selectObservationVoice, selectObservationProjection } from './telemetry';
import { Parameter, Snapshot, Telemetry } from './types';
import { Macros } from './Macros';
import { ResizeHandle } from './ResizeHandle';
import { Dataflow } from './Dataflow';
import { DocumentSession, EditorGroup, openTab, closeTab, reconcileGroups } from './documents';
import { fitSidebars, PanelName, panelShortcut, panelShortcutLabel } from './panelVisibility';
import { planObservation, subscribeObservation } from './probeSelection';

const EMPTY: never[] = [];
const DEFAULT_FONT = "'SFMono-Regular', Consolas, 'Liberation Mono', monospace";
const LAYOUT = { leftWidth: 240, rightWidth: 320, flowHeight: 320, splitRatio: .5, layoutVersion: 2 };
type LayoutPreferences = Partial<typeof LAYOUT>;
const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));
function basename(path: string) { return path.split(/[\\/]/).pop() || path; }
function savedPreference(key: string, fallback: string) { try { return localStorage.getItem(`flows.${key}`) || fallback; } catch { return fallback; } }
function savedVisibility(key: string) { return savedPreference(key, 'true') !== 'false'; }
function savedNumber(key: string, fallback: number) { const value = Number(savedPreference(key, String(fallback))); return Number.isFinite(value) ? value : fallback; }
function migrateFont(value: number, version: number) { return clamp(version >= 2 ? value : Math.max(16, value), 10, 32); }
function usePanelSize<T extends HTMLElement>(ref: RefObject<T>) {
  const [size, setSize] = useState({ width: 0, height: 0 });
  useEffect(() => {
    const element = ref.current; if (!element) return;
    const observer = new ResizeObserver(([entry]) => setSize({ width: entry.contentRect.width, height: entry.contentRect.height }));
    observer.observe(element); return () => observer.disconnect();
  }, [ref]);
  return size;
}

export function App() {
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [error, setError] = useState('');
  const [, refreshDrafts] = useState(0);
  const [groups, setGroups] = useState<EditorGroup[]>([{ tabs: [], active: '' }, { tabs: [], active: '' }]);
  const groupsRef = useRef(groups); groupsRef.current = groups;
  const [focusedGroup, setFocusedGroup] = useState(0), focusedGroupRef = useRef(0);
  const desiredDocument = useRef(''), navigationSequence = useRef(0);
  const [closing, setClosing] = useState<{ group: number; id: string } | null>(null);
  const [closeBusy, setCloseBusy] = useState(false);
  const [focusRequest, setFocusRequest] = useState<{ group: number; id: string } | null>(null);
  const sessionRef = useRef<DocumentSession>();
  const acknowledged = useRef<Snapshot | null>(null);
  const selectedDocument = useRef('');
  const timer = useRef<ReturnType<typeof setTimeout>>();
  const groupStorageKey = useRef('');
  const updateGroups = useCallback((next: EditorGroup[]) => { groupsRef.current = next; setGroups(next); }, []);
  const [theme, setTheme] = useState(savedPreference('theme', 'moss')), [opacity, setOpacity] = useState(Number(savedPreference('opacity', '1')));
  const [preferencesLoaded, setPreferencesLoaded] = useState(!hostAvailable());
  const [split, setSplit] = useState(savedPreference('split', 'none')), [settings, setSettings] = useState(false);
  const [fontFamily, setFontFamily] = useState(savedPreference('fontFamily', DEFAULT_FONT)), [fontSize, setFontSize] = useState(migrateFont(savedNumber('fontSize', 16), savedNumber('layoutVersion', 0)));
  const [leftWidth, setLeftWidth] = useState(clamp(savedNumber('leftWidth', LAYOUT.leftWidth), 120, 520));
  const [rightWidth, setRightWidth] = useState(clamp(savedNumber('rightWidth', LAYOUT.rightWidth), 180, 600));
  const [flowHeight, setFlowHeight] = useState(clamp(savedNumber('flowHeight', LAYOUT.flowHeight), 80, 800));
  const [splitRatio, setSplitRatio] = useState(clamp(savedNumber('splitRatio', .5), .2, .8));
  const [reveal, setReveal] = useState<{ from: number; to: number; nonce: number }>();
  const workspaceRef = useRef<HTMLDivElement>(null), editorWorkspaceRef = useRef<HTMLDivElement>(null), splitRef = useRef<HTMLDivElement>(null);
  const workspaceSize = usePanelSize(workspaceRef), editorWorkspaceSize = usePanelSize(editorWorkspaceRef), splitSize = usePanelSize(splitRef);
  const [rename, setRename] = useState(false), [renameValue, setRenameValue] = useState('');
  const [docked, setDocked] = useState<string[]>([]), [expanded, setExpanded] = useState<string | null>(null);
  const [showExplorer, setShowExplorer] = useState(() => savedVisibility('showExplorer'));
  const [showAssistant, setShowAssistant] = useState(() => savedVisibility('showAssistant'));
  const [showDataflow, setShowDataflow] = useState(() => savedVisibility('showDataflow'));
  const [showKeyboard, setShowKeyboard] = useState(() => savedVisibility('showKeyboard'));
  const mac = /Mac|iPhone|iPad|iPod/.test(navigator.platform);
  const togglePanel = useCallback((panel: PanelName) => {
    if (document.activeElement?.closest(`[data-workspace-panel="${panel}"]`))
      document.querySelector<HTMLElement>('.editor-group.active .editor-document:not([hidden]) .cm-content')?.focus({ preventScroll: true });
    ({ explorer: setShowExplorer, assistant: setShowAssistant, dataflow: setShowDataflow, keyboard: setShowKeyboard })[panel](visible => !visible);
  }, []);
  // Owned here because the enable toggle lives in the header, not the keyboard strip.
  const [typing, setTyping] = useState(false);
  useEffect(() => { const toggle = (event: KeyboardEvent) => { if ((event.metaKey || event.ctrlKey) && !event.shiftKey && !event.altKey && event.key.toLowerCase() === 'k') { event.preventDefault(); event.stopPropagation(); if (!event.repeat) setTyping(value => !value); } }; window.addEventListener('keydown', toggle, true); return () => window.removeEventListener('keydown', toggle, true); }, []);
  const [demoRunning, setDemoRunning] = useState(false), [filter, setFilter] = useState('');
  const [sequencerOpen, setSequencerOpen] = useState(false);
  const [macrosOpen, setMacrosOpen] = useState(false), [macroParameter, setMacroParameter] = useState<Parameter | null>(null);
  const [listenPending, setListenPending] = useState(false);
  const [probeVoice, setProbeVoice] = useState(-1);
  const [probeProjection, setProbeProjection] = useState<Projection>('mid');
  const [probeFailure, setProbeFailure] = useState<{ key: string; message: string }>();
  const [probeFallback, setProbeFallback] = useState<{ key: string; signals: string[]; reason: string }>();
  const windowTransparency = snapshot?.capabilities?.windowTransparency === true;
  const capabilities = snapshot?.capabilities;
  // Browser keys and pickers differ from the native app's; decide before the first snapshot too.
  const web = (capabilities?.host ?? (transportKind() === 'web' ? 'web' : '')) === 'web';
  const browserFiles = capabilities?.fileDialogs === 'browser';
  const [audioPanel, setAudioPanel] = useState(false);
  const auditionQueue = useRef(Promise.resolve()), previousInspector = useRef<{ documentId: string; signal: string | null }>({ documentId: '', signal: null });
  const notifyError = useCallback((value: unknown) => setError(value instanceof Error ? value.message : String(value)), []);
  if (!sessionRef.current) sessionRef.current = new DocumentSession(request, () => {
    const session = sessionRef.current!, next = session.snapshot, previous = acknowledged.current;
    acknowledged.current = next; setSnapshot(next); refreshDrafts(value => value + 1);
    if (!next) return;
    if (!previous) {
      // Native recovery owns the open documents; UI persistence only remembers
      // which group displayed them. Paths scope the layout to this workspace.
      groupStorageKey.current = `editorGroups.${next.workspaceId ?? 'workspace'}`;
      let restored: EditorGroup[] = groupsRef.current;
      try { const value = JSON.parse(savedPreference(groupStorageKey.current, 'null')); if (Array.isArray(value) && value.length === 2 && value.every(group => Array.isArray(group.tabs) && typeof group.active === 'string')) restored = value; } catch { /* Fresh layout. */ }
      let nextGroups = reconcileGroups(restored, session.documents().map(document => document.id), next.document.id);
      if ((next.preferences?.split ?? savedPreference('split', 'none')) === 'none') nextGroups = [{ tabs: [...new Set(nextGroups.flatMap(group => group.tabs))], active: next.document.id }, { tabs: [], active: '' }];
      updateGroups(nextGroups);
      const index = Math.max(0, nextGroups.findIndex(group => group.active === next.document.id));
      focusedGroupRef.current = index; setFocusedGroup(index); desiredDocument.current = next.document.id;
    } else if (session.pendingOperations === 0 && (previous.document.id !== next.document.id || groupsRef.current.some(group => group.tabs.some(id => !session.document(id))))) {
      // Opens from native menus, recovery and the bridge are also tabs.
      const index = focusedGroupRef.current;
      let current = groupsRef.current.map(group => {
        const tabs = group.tabs.filter(id => !!session.document(id));
        const next = group.tabs.slice(group.tabs.indexOf(group.active) + 1).find(id => tabs.includes(id));
        const previous = group.tabs.slice(0, group.tabs.indexOf(group.active)).reverse().find(id => tabs.includes(id));
        return { tabs, active: tabs.includes(group.active) ? group.active : next ?? previous ?? tabs[0] ?? '' };
      });
      if (next.document.id && !current.some(group => group.tabs.includes(next.document.id))) current = openTab(current, index, next.document.id);
      if (next.document.id && previous.document.id !== next.document.id) current = openTab(current, index, next.document.id);
      updateGroups(current); desiredDocument.current = current[index].active;
    }
    if (selectedDocument.current !== next.document.id) {
      selectedDocument.current = next.document.id; setMacrosOpen(false); setMacroParameter(null); setDocked([]); setExpanded(null); setRename(false); setReveal(undefined); resetTelemetry(next.document.revision);
      const voice = next.observation?.voice ?? -1; setProbeVoice(voice); selectObservationVoice(voice); setProbeFailure(undefined); setProbeFallback(undefined);
      const projection = next.observation?.projection ?? 'mid'; setProbeProjection(projection); selectObservationProjection(projection);
    }
    if (next.audio.demoRunning !== undefined) setDemoRunning(next.audio.demoRunning);
  });
  const session = sessionRef.current;
  const receive = useCallback((value: any) => {
    const previous = session.snapshot;
    session.receive(value);
    const next = session.snapshot; if (!next) return;
    if (!previous) {
      if (['moss', 'graphite', 'paper'].includes(next.preferences?.theme || '')) setTheme(next.preferences!.theme!);
      if (typeof next.preferences?.opacity === 'number') setOpacity(Math.max(.7, Math.min(1, next.preferences.opacity)));
      if (['none', 'vertical', 'horizontal'].includes(next.preferences?.split || '')) setSplit(next.preferences!.split!);
      if (next.preferences?.fontFamily) setFontFamily(next.preferences.fontFamily);
      const layout = next.preferences as (Snapshot['preferences'] & LayoutPreferences);
      const storedFont = next.preferences?.fontSize;
      setFontSize(migrateFont(typeof storedFont === 'number' && Number.isFinite(storedFont) ? storedFont : savedNumber('fontSize', 16), layout?.layoutVersion ?? (typeof storedFont === 'number' ? 0 : savedNumber('layoutVersion', 0))));
      if (typeof layout?.leftWidth === 'number' && Number.isFinite(layout.leftWidth)) setLeftWidth(clamp(layout.leftWidth, 120, 520));
      if (typeof layout?.rightWidth === 'number' && Number.isFinite(layout.rightWidth)) setRightWidth(clamp(layout.rightWidth, 180, 600));
      if (typeof layout?.flowHeight === 'number' && Number.isFinite(layout.flowHeight)) setFlowHeight(clamp(layout.flowHeight, 80, 800));
      if (typeof layout?.splitRatio === 'number' && Number.isFinite(layout.splitRatio)) setSplitRatio(clamp(layout.splitRatio, .2, .8));
      if (typeof next.preferences?.showExplorer === 'boolean') setShowExplorer(next.preferences.showExplorer);
      if (typeof next.preferences?.showAssistant === 'boolean') setShowAssistant(next.preferences.showAssistant);
      if (typeof next.preferences?.showDataflow === 'boolean') setShowDataflow(next.preferences.showDataflow);
      if (typeof next.preferences?.showKeyboard === 'boolean') setShowKeyboard(next.preferences.showKeyboard);
      setPreferencesLoaded(true);
    }
  }, [session]);
  const activeId = groups[focusedGroup]?.active || '';
  const text = session.text(activeId), editing = session.drafts.has(activeId), conflict = !!session.drafts.get(activeId)?.conflict;
  const flush = useCallback(async () => {
    clearTimeout(timer.current); timer.current = undefined;
    if (desiredDocument.current) await session.activate(desiredDocument.current);
    else await session.flush();
  }, [session]);
  const activate = useCallback((group: number, id: string, focusEditor = false) => {
    ++navigationSequence.current;
    if (desiredDocument.current !== id) { setMacrosOpen(false); setMacroParameter(null); setExpanded(null); setDocked([]); setReveal(undefined); }
    focusedGroupRef.current = group; setFocusedGroup(group); desiredDocument.current = id;
    updateGroups(id ? openTab(groupsRef.current, group, id) : groupsRef.current); setError('');
    clearTimeout(timer.current); timer.current = undefined;
    const activation = id ? session.activate(id) : Promise.resolve();
    void activation.catch(notifyError);
    if (focusEditor) setFocusRequest({ group, id });
    return activation;
  }, [session, updateGroups, notifyError]);
  const changeDocument = useCallback((id: string, nextText: string, gestureId?: string, gesturePhase?: string) => {
    session.change(id, nextText, gestureId, gesturePhase);
    if (gesturePhase) { void flush().catch(notifyError); return; }
    // Throttle gestures: a continuous drag must reach the engine while moving.
    if (gestureId && timer.current) return;
    clearTimeout(timer.current);
    timer.current = setTimeout(() => { timer.current = undefined; void flush().catch(notifyError); }, gestureId ? 25 : 80);
  }, [session, flush, notifyError]);
  const perform = useCallback(async (method: string, params: Record<string, unknown> = {}, documentId = desiredDocument.current) => {
    try {
      clearTimeout(timer.current); timer.current = undefined;
      const isOpen = ['document.open', 'document.new', 'project.import', 'project.importText'].includes(method);
      const group = focusedGroupRef.current, navigation = isOpen ? ++navigationSequence.current : navigationSequence.current;
      const result = await session.execute(method, params, isOpen ? undefined : documentId);
      const next = session.snapshot;
      if (isOpen && next?.document.id) {
        const currentIntent = navigation === navigationSequence.current && focusedGroupRef.current === group;
        const nextGroups = openTab(groupsRef.current, group, next.document.id);
        if (!currentIntent && groupsRef.current[group].active) nextGroups[group] = { ...nextGroups[group], active: groupsRef.current[group].active };
        updateGroups(nextGroups);
        if (currentIntent) { desiredDocument.current = next.document.id; setFocusRequest({ group, id: next.document.id }); }
        else if (desiredDocument.current) await session.activate(desiredDocument.current);
      }
      if (method === 'document.rename' && next?.document.id) {
        const renamed = next.document.id;
        updateGroups(groupsRef.current.map(entry => ({ tabs: [...new Set(entry.tabs.map(id => id === documentId ? renamed : id))], active: entry.active === documentId ? renamed : entry.active })));
        if (desiredDocument.current === documentId) desiredDocument.current = renamed;
      }
      return result;
    } catch (cause) { notifyError(cause); return undefined; }
  }, [session, updateGroups, notifyError]);
  const compile = useCallback(() => perform('compile'), [perform]);
  // Browser hosts pick files in the page: the picker must open inside the click.
  const importPackage = useCallback(() => {
    if (!browserFiles) { void perform('project.import'); return; }
    void pickFile('.flowsinst,application/json').then(async file => {
      if (!file) return;
      if (file.size > 96 * 1024 * 1024) throw new Error('Instrument packages are limited to 96 MiB.');
      await perform('project.importText', { text: await file.text() });
    }).catch(notifyError);
  }, [browserFiles, perform, notifyError]);
  const exportPackage = useCallback(() => {
    if (!browserFiles) { void perform('project.export'); return; }
    void perform('project.exportText').then(text => {
      if (typeof text !== 'string') return;
      const name = basename(session.snapshot?.document.path || 'Instrument').replace(/\.flows$/i, '') || 'Instrument';
      download(`${name}.flowsinst`, text, 'application/json');
    }).catch(notifyError);
  }, [browserFiles, perform, session, notifyError]);
  const importSample = useCallback(() => {
    if (!browserFiles) { void perform('sample.import'); return; }
    void pickFile(audioAccept).then(async file => {
      if (!file) return;
      const audio = await importableAudio(file.name, new Uint8Array(await file.arrayBuffer()));
      await perform('workspace.import', { folder: 'Samples', name: audio.name, data: bytesToBase64(audio.bytes) });
    }).catch(notifyError);
  }, [browserFiles, perform, notifyError]);
  useEffect(() => {
    const open = () => { setSettings(false); setAudioPanel(true); };
    window.addEventListener('flows:audio-settings', open);
    return () => window.removeEventListener('flows:audio-settings', open);
  }, []);
  useEffect(() => {
    if (!web) return;
    // Recovery keeps drafts, but the last edits may still be in flight.
    const guard = (event: BeforeUnloadEvent) => {
      if (!session.drafts.size && !session.documents().some(document => session.dirty(document.id))) return;
      event.preventDefault(); event.returnValue = '';
    };
    window.addEventListener('beforeunload', guard);
    return () => window.removeEventListener('beforeunload', guard);
  }, [web, session]);
  const splitEditors = useCallback((orientation: 'vertical' | 'horizontal') => {
    if (split === orientation) {
      const all = [...new Set(groupsRef.current.flatMap(group => group.tabs))];
      const id = groupsRef.current[focusedGroupRef.current].active || all[0] || '';
      updateGroups([{ tabs: all, active: id }, { tabs: [], active: '' }]); setSplit('none'); activate(0, id, true);
    } else {
      const id = groupsRef.current[focusedGroupRef.current].active;
      if (!groupsRef.current[1].tabs.length && id) updateGroups(openTab(groupsRef.current, 1, id));
      setSplit(orientation); activate(1, groupsRef.current[1].active || id, true);
    }
  }, [split, activate, updateGroups]);
  const finishClose = useCallback(async (group: number, id: string, choice?: 'save' | 'discard') => {
    setCloseBusy(true);
    try {
      const duplicate = groupsRef.current.some((entry, index) => index !== group && entry.tabs.includes(id));
      if (!duplicate) await session.close(id, choice);
      const next = closeTab(groupsRef.current, group, id); updateGroups(next); setClosing(null);
      let index = focusedGroupRef.current;
      if (!next[index].active && next[1 - index].active && split !== 'none') index = 1 - index;
      const selected = next[index].active;
      if (selected) activate(index, selected, true);
      else { desiredDocument.current = ''; focusedGroupRef.current = index; setFocusedGroup(index); }
    } catch (cause) { notifyError(cause); } finally { setCloseBusy(false); }
  }, [session, split, updateGroups, activate, notifyError]);
  const requestClose = useCallback((group: number, id: string) => {
    if (!id) return;
    const duplicate = groupsRef.current.some((entry, index) => index !== group && entry.tabs.includes(id));
    if (!duplicate && session.dirty(id)) setClosing({ group, id });
    else void finishClose(group, id);
  }, [session, finishClose]);
  const listen = useCallback((signal: string, enabled: boolean) => {
    const documentId = desiredDocument.current; if (!documentId) return;
    setListenPending(true);
    const job = auditionQueue.current.then(async () => {
      if (desiredDocument.current !== documentId) return;
      await perform('probe.listen', { signal, enabled, documentId }, documentId);
    });
    auditionQueue.current = job;
    void job.finally(() => { if (auditionQueue.current === job) setListenPending(false); });
  }, [perform]);
  useEffect(() => {
    const previous = previousInspector.current;
    previousInspector.current = { documentId: activeId, signal: expanded };
    // Native activation releases the outgoing audition. An old modal closing
    // must never disable a new instrument's audition with the same signal name.
    if (previous.documentId !== activeId || previous.signal === expanded || acknowledged.current?.document.id !== activeId) return;
    const audition = acknowledged.current?.audio.audition;
    if (audition && audition !== expanded) listen(audition, false);
  }, [expanded, activeId, listen]);
  useEffect(() => {
    if (!hostAvailable()) return;
    const state = subscribe<Snapshot>('flowsState', receive), audio = subscribe<Telemetry>('flowsTelemetry', publishTelemetry);
    void request('session.get').then(receive).catch(notifyError);
    return () => { state(); audio(); clearTimeout(timer.current); timer.current = undefined; };
  }, [receive, notifyError]);
  useEffect(() => {
    document.documentElement.dataset.theme = theme; document.documentElement.style.setProperty('--app-opacity', String(windowTransparency ? opacity : 1));
    document.documentElement.style.setProperty('--editor-font', `${fontFamily || DEFAULT_FONT}, monospace`); document.documentElement.style.setProperty('--editor-size', `${14 * fontSize / 16}px`);
    document.documentElement.style.fontSize = `${12 * fontSize / 16}px`;
  }, [theme, opacity, fontFamily, fontSize, windowTransparency]);
  useEffect(() => {
    if (hostAvailable() && !preferencesLoaded) return;
    const preferences = { theme, opacity, split, fontFamily, fontSize, leftWidth, rightWidth, flowHeight, splitRatio, layoutVersion: 2, showExplorer, showAssistant, showDataflow, showKeyboard };
    const timer = setTimeout(() => {
      try { for (const [key, value] of Object.entries(preferences)) localStorage.setItem(`flows.${key}`, String(value)); } catch { /* Native preferences remain available. */ }
      if (hostAvailable()) void request('preferences.set', preferences).catch(notifyError);
    }, 180);
    return () => clearTimeout(timer);
  }, [theme, opacity, split, fontFamily, fontSize, leftWidth, rightWidth, flowHeight, splitRatio, showExplorer, showAssistant, showDataflow, showKeyboard, preferencesLoaded, notifyError]);
  useEffect(() => {
    if (!focusRequest || focusedGroup !== focusRequest.group || desiredDocument.current !== focusRequest.id || groups[focusRequest.group].active !== focusRequest.id) return;
    // Focus only the requested document after React has committed its visibility and mounted CodeMirror.
    // A RAF querying whichever view is currently visible can reactivate the old tab.
    document.querySelector<HTMLElement>(`[data-editor-group="${focusRequest.group}"] [data-editor-document="${CSS.escape(focusRequest.id)}"]:not([hidden]) .cm-content`)?.focus({ preventScroll: true });
  }, [focusRequest, focusedGroup, groups[0].active, groups[1].active, split]);
  useEffect(() => {
    for (const element of document.querySelectorAll<HTMLElement>('.editor-group .editor-tab.active')) element.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }, [groups[0].active, groups[1].active, split]);
  useEffect(() => {
    if (groupStorageKey.current) { try { localStorage.setItem(`flows.${groupStorageKey.current}`, JSON.stringify(groups)); } catch { /* Native documents remain available. */ } }
  }, [groups]);
  useEffect(() => {
    const shortcuts = (event: KeyboardEvent) => {
      const shortcut = editorShortcut(event, { mac, web });
      if (!shortcut) return;
      const action = () => {
        const index = focusedGroupRef.current, group = groupsRef.current[index];
        if (shortcut === 'close') requestClose(index, group.active);
        else if (shortcut === 'split') splitEditors('vertical');
        else if (shortcut === 'focus-first' || shortcut === 'focus-second') {
          const target = shortcut === 'focus-first' ? 0 : 1;
          if (target === 0 || split !== 'none') activate(target, groupsRef.current[target].active, true);
        } else if (group.tabs.length) {
          const offset = shortcut === 'previous-tab' ? -1 : 1;
          activate(index, group.tabs[(group.tabs.indexOf(group.active) + offset + group.tabs.length) % group.tabs.length], true);
        }
      };
      event.preventDefault(); event.stopPropagation(); if (!event.repeat) action();
    };
    window.addEventListener('keydown', shortcuts, true); return () => window.removeEventListener('keydown', shortcuts, true);
  }, [mac, web, activate, split, splitEditors, requestClose]);
  useEffect(() => {
    const zoom = (event: KeyboardEvent) => {
      const change = textSizeShortcut(event, { web });
      if (!change) return;
      event.preventDefault(); event.stopPropagation();
      setFontSize(value => change === 'reset' ? 16 : clamp(value + (change === 'smaller' ? -1 : 1), 10, 32));
    };
    window.addEventListener('keydown', zoom, true); return () => window.removeEventListener('keydown', zoom, true);
  }, [web]);
  useEffect(() => {
    const shortcut = (event: KeyboardEvent) => {
      const panel = panelShortcut(event, mac); if (!panel) return;
      event.preventDefault(); event.stopPropagation();
      if (!event.repeat) togglePanel(panel);
    };
    window.addEventListener('keydown', shortcut, true);
    return () => window.removeEventListener('keydown', shortcut, true);
  }, [mac, togglePanel]);
  useEffect(() => {
    // A keyboard shortcut can hide a focused text field. Move focus back to
    // the source instead of sending subsequent typing into an invisible panel.
    if (document.activeElement?.closest('[data-workspace-panel][hidden]'))
      document.querySelector<HTMLElement>('.editor-group.active .editor-document:not([hidden]) .cm-content')?.focus({ preventScroll: true });
  }, [showExplorer, showAssistant, showDataflow, showKeyboard]);
  // Native snapshots include changing meters. A stable selection key avoids
  // resetting the capture window every time only a meter or macro value changes.
  const voiceLimit = Math.max(1, Math.min(32, snapshot?.compile.voiceLimit ?? 32));
  const observationVoice = probeVoice < voiceLimit ? probeVoice : -1;
  const changeObservationVoice = useCallback((voice: number) => { selectObservationVoice(voice); setProbeVoice(voice); }, []);
  useEffect(() => { selectObservationVoice(observationVoice); }, [observationVoice]);
  const changeObservationProjection = useCallback((projection: Projection) => { selectObservationProjection(projection); setProbeProjection(projection); }, []);
  const observationPlan = planObservation(snapshot?.compile.signals ?? [], [...(expanded ? [expanded] : []), ...docked]);
  const observedNames = new Set(snapshot?.compile.signals?.map(signal => signal.name));
  const observationFocus = [expanded, ...docked, snapshot?.compile.signals?.find(signal => signal.kind === 'analysis')?.name, snapshot?.compile.signals?.[0]?.name].find((name): name is string => !!name && observedNames.has(name));
  const probeSelectionKey = JSON.stringify({ documentId: snapshot?.document.id, status: snapshot?.compile.status, revision: snapshot?.compile.revision, programRevision: snapshot?.audio.programRevision, signals: observationPlan.signals, voice: observationVoice, projection: probeProjection, focus: observationFocus });
  useEffect(() => {
    const selection = JSON.parse(probeSelectionKey) as { documentId?: string; signals: string[]; voice: number; projection: Projection; status?: string; focus?: string };
    let current = true;
    const stillActive = () => current && selection.documentId === desiredDocument.current && selection.documentId === acknowledged.current?.document.id;
    if (hostAvailable() && selection.status === 'ready' && selection.documentId
      && stillActive()) {
      void subscribeObservation(signals => {
        if (!stillActive()) return Promise.reject(new Error('Observation request was superseded'));
        return request('probe.set', { signals, voice: selection.voice < 0 ? 'mixed' : selection.voice, projection: selection.projection, documentId: selection.documentId });
      }, selection.signals, selection.focus)
        .then(({ result, signals, reduced }) => {
          if (stillActive()) {
            setProbeFailure(undefined);
            setProbeFallback(reduced ? { key: probeSelectionKey, signals, reason: reduced } : undefined);
            receive(result);
          }
        })
        .catch(error => {
          if (stillActive()) {
            setProbeFallback(undefined);
            setProbeFailure({ key: probeSelectionKey, message: error instanceof Error ? error.message : String(error) });
          }
        });
    }
    return () => { current = false; };
  }, [probeSelectionKey, receive]);
  const probeError = probeFailure?.key === probeSelectionKey ? probeFailure.message : undefined;
  const reducedObservation = probeFallback?.key === probeSelectionKey ? probeFallback : undefined;
  const observationSkipped = { ...observationPlan.skipped };
  if (reducedObservation) for (const name of observedNames) if (!reducedObservation.signals.includes(name))
    observationSkipped[name] = 'Automatic observation was reduced to one view. Select this signal to observe it.';
  const observationNotice = reducedObservation ? `Automatic observation reduced: ${reducedObservation.reason}` : undefined;
  useEffect(() => {
    const escape = (event: KeyboardEvent) => { if (event.key === 'Escape') { setExpanded(null); setSettings(false); setAudioPanel(false); setSequencerOpen(false); setMacrosOpen(false); setRename(false); if (!closeBusy) setClosing(null); } };
    window.addEventListener('keydown', escape); return () => window.removeEventListener('keydown', escape);
  }, [closeBusy]);
  const revealSource = useCallback((from: number, to: number) => {
    const state = acknowledged.current;
    if (!state || state.document.text !== text || state.compile.status !== 'ready'
      || state.compile.revision !== undefined && state.compile.revision !== state.document.revision
      || !Number.isInteger(from) || !Number.isInteger(to) || from < 0 || to <= from || to > text.length) return;
    setReveal(current => ({ from, to, nonce: (current?.nonce ?? 0) + 1 }));
  }, [text]);
  const inspect = useCallback((name: string) => {
    setDocked(current => [...new Set([name, ...current])]); setExpanded(name);
    const state = acknowledged.current;
    const signal = state?.document.text === text ? state.compile.signals?.find(signal => signal.name === name) : undefined;
    if (signal) revealSource(signal.from, signal.to);
  }, [text, revealSource]);
  const documentState = session.document(activeId), connected = hostAvailable() && Boolean(snapshot);
  const instrumentReady = connected && !!activeId && snapshot?.document.id === activeId;
  const keyboardReady = instrumentReady && !editing && snapshot?.compile.status === 'ready' && snapshot.compile.revision === documentState?.revision;
  const sequencerOwnerId = snapshot?.sequencer?.ownerDocumentId || activeId;
  const sequencerSound = snapshot?.sequencer?.sound;
  const sequencerCanPlay = connected;
  const sequencerSoundName = sequencerSound?.kind === 'lanes' ? 'lane instruments' : sequencerSound && sequencerSound.kind !== 'active' ? basename(sequencerSound.path) : 'the active instrument';
  const flowDocumentId = documentState?.id;
  const flowChange = useCallback((nextText: string, gestureId?: string, gesturePhase?: string) => {
    if (!flowDocumentId || acknowledged.current?.document.id !== flowDocumentId || desiredDocument.current !== flowDocumentId) return;
    changeDocument(flowDocumentId, nextText, gestureId, gesturePhase);
  }, [flowDocumentId, changeDocument]);
  const metadataCurrent = instrumentReady && documentState?.text === text && (snapshot?.compile.revision === undefined || snapshot.compile.revision === documentState.revision);
  const parameters = metadataCurrent ? snapshot?.compile.parameters ?? EMPTY : EMPTY;
  const diagnosticList = instrumentReady ? snapshot?.compile.diagnostics || EMPTY : EMPTY;
  const compileStatus = !activeId ? 'No instrument' : !instrumentReady ? 'Switching instrument' : editing ? 'draft' : snapshot?.compile.status || 'disconnected';
  const problem = diagnosticList.filter(diagnostic => diagnostic.severity !== 'info').length;
  const editorProps = { onUndo: () => { void perform('document.undo'); }, onRedo: () => { void perform('document.redo'); }, onSave: () => { void perform('document.save'); } };
  const signalProps = (name: string) => ({ documentId: activeId, signal: snapshot?.compile.signals?.find(signal => signal.name === name), listening: snapshot?.audio.audition === name, listenPending, error: probeError || observationSkipped[name], voice: observationVoice, voiceLimit, onVoiceChange: changeObservationVoice, projection: probeProjection, onProjectionChange: changeObservationProjection, onListen: connected ? (enabled: boolean) => listen(name, enabled) : undefined });
  const widths = fitSidebars(workspaceSize.width || innerWidth, leftWidth, rightWidth, showExplorer, showAssistant);
  const panelControls: { panel: PanelName; label: string; icon: string; visible: boolean; controls: string }[] = [
    { panel: 'explorer', label: 'Explorer', icon: 'sidebar-left', visible: showExplorer, controls: 'explorer-panel' },
    { panel: 'assistant', label: 'AI assistant', icon: 'sidebar-right', visible: showAssistant, controls: 'assistant-panel' },
    { panel: 'dataflow', label: 'Signal flow', icon: 'signal-flow', visible: showDataflow, controls: 'dataflow-panel' },
    { panel: 'keyboard', label: 'Piano keyboard', icon: 'keyboard', visible: showKeyboard, controls: 'keyboard-panel' },
  ];
  const flowMax = Math.max(80, Math.min(800, (editorWorkspaceSize.height || 560) - 166));
  const flowMin = 80, visibleFlowHeight = clamp(flowHeight, flowMin, flowMax);
  const splitExtent = Math.max(1, (split === 'horizontal' ? splitSize.height : splitSize.width) - 6);
  const splitMin = Math.min(.5, Math.max(.2, (split === 'horizontal' ? 80 : 160) / splitExtent));
  const visibleSplitRatio = clamp(splitRatio, splitMin, 1 - splitMin);

  return <main className="flows-app">
    <header className="app-header"><button className={`transport-play ${demoRunning ? 'playing' : ''}`} disabled={!connected || (!demoRunning && !sequencerCanPlay)} title={demoRunning ? 'Stop step sequencer' : `Play step sequencer with ${sequencerSoundName}`} onClick={() => void perform('sequencer.play', { documentId: activeId, playing: !demoRunning }, activeId)}><Icon name={demoRunning ? 'stop' : 'play'} size={13}/></button><TempoControl bpm={snapshot?.audio.bpm ?? 138} host={snapshot?.audio.tempoSource === 'host'} disabled={!connected} onChange={bpm => void perform('transport.set', { bpm })}/><button className={`sequencer-toggle ${sequencerOpen ? 'selected' : ''}`} disabled={!connected || !snapshot?.sequencer} title="Choose a sound and edit notes, rhythm and playback modes" aria-expanded={sequencerOpen} aria-controls="step-sequencer" onClick={() => { setSettings(false); setSequencerOpen(!sequencerOpen); }}><Icon name="rows" size={15}/><span>Step sequencer</span><small>{snapshot?.sequencer?.length ?? 16}</small></button><span className="small-divider"/><div className="panel-toggles" role="group" aria-label="Workspace panels">{panelControls.map(({ panel, label, icon, visible, controls }) => { const action = `${visible ? 'Hide' : 'Show'} ${label}`, shortcut = panelShortcutLabel(panel, mac); return <button key={panel} className={`icon-button panel-toggle ${visible ? 'selected' : ''} ${panel === 'keyboard' ? 'keyboard-collapse' : ''}`} data-panel-toggle={panel} title={`${action} · ${shortcut}`} aria-label={`${action} · ${shortcut}`} aria-pressed={visible} aria-expanded={visible} aria-controls={controls} onClick={() => togglePanel(panel)}><Icon name={icon} size={16}/></button>; })}</div><div className="spacer"/><div className={`program-state ${['compiled', 'success', 'ready', 'valid'].includes(compileStatus) ? 'ready' : problem ? 'problem' : ''}`}><i/>{compileStatus === 'disconnected' ? 'Engine disconnected' : editing ? 'Editing source' : problem ? `${problem} diagnostic${problem === 1 ? '' : 's'}` : compileStatus}</div><button className={`icon-button typing-toggle mode-toggle ${typing ? 'selected' : ''}`} title={typing ? 'Perform mode · ⌘K · Drag parameters and play keys' : 'Edit mode · ⌘K · Edit source'} aria-pressed={typing} disabled={!connected} onClick={() => setTyping(!typing)}><Icon name="computer-keyboard"/><span>{typing ? 'Perform' : 'Edit'}</span></button><button className="icon-button" title="Appearance" onClick={() => setSettings(!settings)}><Icon name="settings"/></button><span className="small-divider"/><div className="header-audio-center"><AudioReadout/></div><span className="small-divider"/><button className="audio-device" title="Audio & MIDI settings" disabled={!connected} aria-expanded={capabilities?.audioSettings === 'in-app' ? audioPanel : undefined} onClick={() => { if (capabilities?.audioSettings === 'in-app') { setSettings(false); setAudioPanel(open => !open); } else void perform('audio.settings'); }}><span className={`connection-dot ${snapshot?.audio.running ? 'active' : ''}`}/>{web && !snapshot?.audio.running ? 'Sound off' : snapshot?.audio.sampleRate ? `${(snapshot.audio.sampleRate / 1000).toFixed(1)} kHz` : 'Audio disconnected'}{!(web && !snapshot?.audio.running) && <span> / {snapshot?.audio.blockSize ?? '—'}</span>}<Icon name="settings" size={12}/></button></header>
    {web && <WebNotices onExport={exportPackage}/>}
    <div className="workspace" ref={workspaceRef} style={{ gridTemplateColumns: `${widths.left}px ${showExplorer ? 6 : 0}px minmax(0,1fr) ${showAssistant ? 6 : 0}px ${widths.right}px` }}>
      <aside className="files-panel" id="explorer-panel" data-workspace-panel="explorer" hidden={!showExplorer}><header className="panel-heading"><span><Icon name="folder"/> EXPLORER</span><div className="explorer-actions"><button className="icon-button" title="New instrument" disabled={!connected} onClick={() => void perform('document.new')}><Icon name="add" size={15}/></button><button className="icon-button" title="Import instrument package" disabled={!connected} onClick={importPackage}><Icon name="import" size={14}/></button></div></header><div className="file-search"><input value={filter} onChange={event => setFilter(event.target.value)} placeholder="Find instruments or samples…" aria-label="Filter instruments and samples"/><span>⌕</span></div><Explorer onImportFiles={async (files, folder) => { for (const file of files) { try { if (file.size > 64 * 1024 * 1024) throw new Error(`${file.name}: maximum file size is 64 MiB`); const data = await new Promise<string>((resolve, reject) => { const reader = new FileReader(); reader.onload = () => resolve(String(reader.result).split(',')[1]); reader.onerror = () => reject(new Error(`Could not read ${file.name}`)); reader.readAsDataURL(file); }); await perform('workspace.import', { folder, name: file.name, data }); } catch (error) { notifyError(error); } } }} samples={snapshot?.samples || EMPTY} onImportSample={importSample} onSampleInstrument={samplePath => void perform('document.new', { samplePath })} files={snapshot?.files || EMPTY} query={filter} activePath={documentState?.path || ''} connected={connected} workspaceId={snapshot?.workspaceId} onOpen={path => void perform('document.open', { path })}/><div className="explorer-bottom"><button className="text-button" disabled={!connected} onClick={exportPackage}><Icon name="export" size={14}/> Export instrument</button><div className="explorer-note">Code is the instrument.<br/>Listen at every step.</div></div></aside>
      {showExplorer && <ResizeHandle label="Resize explorer" orientation="vertical" controls="explorer-panel" value={widths.left} min={widths.minLeft} max={Math.min(520, widths.budget - widths.right)} defaultValue={LAYOUT.leftWidth} onReset={() => setLeftWidth(LAYOUT.leftWidth)} onChange={setLeftWidth}/>}
      <section className="instrument-panel"><div className="document-toolbar"><div className="workspace-instrument"><Icon name="sound" size={15}/><span>{documentState ? basename(documentState.path) : 'No instrument'}</span><small>{instrumentReady ? 'ACTIVE INSTRUMENT' : 'WORKSPACE'}</small></div><div className="spacer"/><button className="icon-button" disabled={!connected || typing} title="Undo · ⌘ Z" onClick={editorProps.onUndo}><Icon name="undo" size={14}/></button><button className="icon-button" disabled={!connected || typing} title="Redo · ⌘ ⇧ Z" onClick={editorProps.onRedo}><Icon name="redo" size={14}/></button><span className="small-divider"/><button className={`icon-button ${split === 'vertical' ? 'selected' : ''}`} title="Split side by side · ⌘ \" onClick={() => splitEditors('vertical')}><Icon name="split" size={15}/></button><button className={`icon-button ${split === 'horizontal' ? 'selected' : ''}`} title="Split above and below" onClick={() => splitEditors('horizontal')}><Icon name="rows" size={15}/></button><button className={`icon-button ${macrosOpen ? 'selected' : ''}`} disabled={!instrumentReady} title="Host automation · export parameters" onClick={() => setMacrosOpen(!macrosOpen)}><Icon name="export" size={15}/></button><button className="icon-button" title="Rename instrument" disabled={!connected} onClick={() => { setRenameValue(basename(documentState?.path || 'Instrument.flows')); setRename(true); }}><Icon name="rename" size={14}/></button><button className="icon-button" title="Save · ⌘ S" disabled={!connected} onClick={editorProps.onSave}><Icon name="save" size={15}/></button><button className="compile-button" title="Compile · ⌘ Enter" disabled={!connected} onClick={() => void compile()}><Icon name="code" size={14}/> Compile <kbd>⌘ ↵</kbd></button></div>
      {error && <div className="error-banner"><span>{error}</span><div className="spacer"/>{conflict && <><button onClick={() => { session.reload(activeId); setError(''); }}>Reload native source</button><button onClick={() => { setError(''); void session.applyDraft(activeId).catch(notifyError); }}>Apply my draft</button></>}<button className="icon-button" title="Dismiss error" onClick={() => setError('')}><Icon name="close" size={13}/></button></div>}
      <div className="editor-workspace" ref={editorWorkspaceRef}><div className={`editor-splits ${split}`} ref={splitRef}>{groups.map((group, index) => {
        if (index === 1 && split === 'none') return null;
        return <div className="editor-group-slot" key={index} style={{ display: 'contents' }}>
          {index === 1 && <ResizeHandle label="Resize split editors" orientation={split === 'horizontal' ? 'horizontal' : 'vertical'} controls="editor-first editor-second" value={visibleSplitRatio * 100} min={splitMin * 100} max={(1 - splitMin) * 100} defaultValue={50} onReset={() => setSplitRatio(LAYOUT.splitRatio)} step={2} unit="%" pixelsPerUnit={splitExtent / 100} onChange={value => setSplitRatio(value / 100)}/>}
          <div className={`editor-pane editor-group ${focusedGroup === index ? 'active' : ''}`} data-editor-group={index} id={index === 0 ? 'editor-first' : 'editor-second'} style={{ flex: `${split === 'none' ? 1 : index === 0 ? visibleSplitRatio : 1 - visibleSplitRatio} 1 0` }}>
            <div className="editor-group-tabs" role="tablist" aria-label={`Editor group ${index + 1}`}>
              {group.tabs.map(id => { const entry = session.document(id); if (!entry) return null; return <div className={`editor-tab ${group.active === id ? 'active' : ''}`} key={id} data-document-id={id} role="tab" aria-selected={group.active === id} tabIndex={group.active === id ? 0 : -1} title={entry.path} onClick={() => activate(index, id, true)} onKeyDown={event => { if (event.target === event.currentTarget && (event.key === 'Enter' || event.key === ' ')) { event.preventDefault(); activate(index, id, true); } }}><Icon name="code" size={13}/><span data-tab-label>{basename(entry.path)}</span>{session.dirty(id) && <i className="unsaved-dot"/>}<button className="tab-close icon-button" title={`Close ${basename(entry.path)}`} aria-label={`Close ${basename(entry.path)}`} onClick={event => { event.stopPropagation(); requestClose(index, id); }}><Icon name="close" size={12}/></button></div>; })}
              <div className="spacer"/><span className="group-number" title={`Focus editor group ${index + 1} · ⌘ ${index + 1}`}>{index + 1}</span>
            </div>
            {group.tabs.map(id => {
              const entry = session.document(id); if (!entry) return null;
              const source = session.text(id), visible = group.active === id;
              const live = visible && activeId === id && snapshot?.document.id === id;
              const compiled = live ? snapshot.compile : entry.compile;
              const current = source === entry.text && (compiled?.revision === undefined || compiled.revision === entry.revision);
              return <div key={id} className="editor-document" data-editor-document={id} hidden={!visible} onFocusCapture={() => { if (focusedGroupRef.current !== index || desiredDocument.current !== id) activate(index, id); }} onPointerDownCapture={() => { if (focusedGroupRef.current !== index || desiredDocument.current !== id) activate(index, id); }}>
                <div className="editor-breadcrumb"><span>Instrument</span><Icon name="chevron" size={10}/><span>{basename(entry.path)}</span><span className="spacer"/><span className={`editor-live-state ${live ? current && compiled?.status === 'error' ? 'invalid' : 'live' : ''}`}>{live ? current && compiled?.status === 'error' ? '● MUTED · COMPILE ERROR' : '● ACTIVE' : 'FOCUS TO PLAY'}</span></div>
                <Editor perform={typing} text={source} documentId={id} samples={snapshot?.samples} signals={current ? compiled?.signals ?? EMPTY : EMPTY} parameters={current ? compiled?.parameters ?? EMPTY : EMPTY} choices={current ? compiled?.choices ?? EMPTY : EMPTY} diagnostics={current ? compiled?.diagnostics ?? EMPTY : EMPTY} connected={connected} metadataPending={!current || compiled?.status !== 'ready'} live={live} visible={visible} reveal={focusedGroup === index && live ? reveal : undefined}
                  onChange={(source, gesture, phase) => changeDocument(id, source, gesture, phase)} onInspect={name => { void activate(index, id).then(() => { if (desiredDocument.current === id) { setDocked(current => [...new Set([...current, name])].slice(-8)); setExpanded(name); } }).catch(notifyError); }} onExport={parameter => { void activate(index, id).then(() => { if (desiredDocument.current === id) { setMacroParameter(parameter); setMacrosOpen(true); } }).catch(notifyError); }}
                  onUndo={() => { void perform('document.undo', {}, id); }} onRedo={() => { void perform('document.redo', {}, id); }} onSave={() => { void perform('document.save', {}, id); }} onCompile={() => { void perform('compile', {}, id); }}/>
              </div>;
            })}
            {!group.active && <div className="empty-editor" onPointerDown={() => { focusedGroupRef.current = index; setFocusedGroup(index); }}><Icon name="code" size={34}/><h2>{connected ? 'Open an instrument' : 'Your sound starts here.'}</h2><p>{connected ? 'Choose a file from Explorer or create a new instrument.' : 'Open this editor inside Flows to connect audio.'}</p><button className="secondary-button" disabled={!connected} onClick={() => { focusedGroupRef.current = index; setFocusedGroup(index); void perform('document.new'); }}><Icon name="add" size={14}/> New instrument</button><small>⌘ \ split · ⌘ 1 / 2 focus group · ⌘ W close tab</small></div>}
          </div>
        </div>;
      })}</div>
      {problem > 0 && <div className="diagnostics-list">{diagnosticList.slice(0, 3).map((diagnostic, index) => <div key={index}><span>{diagnostic.severity || 'error'}</span>{diagnostic.line ? `Line ${diagnostic.line} · ` : ''}{diagnostic.message}</div>)}</div>}
      {showDataflow && <ResizeHandle label="Resize dataflow" orientation="horizontal" controls="dataflow-panel" value={visibleFlowHeight} min={flowMin} max={flowMax} defaultValue={LAYOUT.flowHeight} onReset={() => setFlowHeight(LAYOUT.flowHeight)} reverse onChange={setFlowHeight}/>}
      <div className="flow-panel" id="dataflow-panel" data-workspace-panel="dataflow" hidden={!showDataflow} style={{ height: visibleFlowHeight }}><Dataflow fontScale={fontSize / 16} graph={metadataCurrent ? snapshot?.compile.graph : null} parameters={parameters} choices={metadataCurrent ? snapshot?.compile.choices ?? EMPTY : EMPTY} source={text} documentId={documentState?.id || 'disconnected'} editable={instrumentReady && !conflict} status={compileStatus} audition={snapshot?.audio.audition} listenPending={listenPending} signals={snapshot?.compile.signals} observation={{ voice: observationVoice, limit: voiceLimit, roots: observationPlan.roots, skipped: observationSkipped, error: probeError, notice: observationNotice, onVoiceChange: changeObservationVoice, projection: probeProjection, onProjectionChange: changeObservationProjection }} onChange={flowChange} onInspect={inspect} onReveal={revealSource} onListen={listen} onObserve={setDocked}/></div></div>
      <div className="keyboard-panel" id="keyboard-panel" data-workspace-panel="keyboard" hidden={!showKeyboard}><Keyboard documentId={instrumentReady ? activeId : ''} connected={keyboardReady} typing={typing} visible={showKeyboard} onError={notifyError}/></div></section>
      {showAssistant && <ResizeHandle label="Resize assistant" orientation="vertical" controls="assistant-panel" value={widths.right} min={widths.minRight} max={Math.min(600, widths.budget - widths.left)} defaultValue={LAYOUT.rightWidth} onReset={() => setRightWidth(LAYOUT.rightWidth)} reverse onChange={setRightWidth}/>}
      <div className="agent-region" id="assistant-panel" data-workspace-panel="assistant" hidden={!showAssistant}><Agent snapshot={snapshot} connected={connected} flush={flush} onResult={receive} onError={notifyError}/></div>
    </div>
    {sequencerOpen && connected && snapshot?.sequencer && <StepSequencer key={sequencerOwnerId} documentId={sequencerOwnerId} state={snapshot.sequencer}
      bpm={snapshot.audio.bpm ?? 138} hostTempo={snapshot.audio.tempoSource === 'host'} instrumentName={basename(documentState?.path || 'Instrument')}
      instruments={snapshot.files} samples={snapshot.samples ?? EMPTY}
      disabled={!connected} canPlay={sequencerCanPlay} onClose={() => setSequencerOpen(false)}
      onChange={async pattern => { const current = acknowledged.current; if (!current?.sequencer || (current.sequencer.ownerDocumentId || current.document.id) !== sequencerOwnerId) throw new Error('The sequencer pattern changed.'); if (current.document.id !== activeId || desiredDocument.current !== activeId) throw new Error('The active instrument changed.'); if (await perform('sequencer.set', { documentId: activeId, pattern, baseRevision: current.sequencer.revision }, activeId) === undefined) throw new Error('Could not update the pattern.'); }}
      onTempoChange={bpm => perform('transport.set', { bpm }, activeId)}
      onPlayingChange={playing => perform('sequencer.play', { documentId: activeId, playing }, activeId)}/>}
    {settings && <div className="appearance-popover"><header><span>Appearance</span><button className="icon-button" title="Close appearance" onClick={() => setSettings(false)}><Icon name="close" size={14}/></button></header><label>Theme<select value={theme} onChange={event => setTheme(event.target.value)}><option value="moss">Moss</option><option value="graphite">Graphite</option><option value="paper">Paper</option></select></label><label>Editor font<input aria-label="Editor font family" value={fontFamily} list="editor-fonts" onChange={event => setFontFamily(event.target.value)} placeholder={DEFAULT_FONT}/><datalist id="editor-fonts"><option value={DEFAULT_FONT}/><option value="'JetBrains Mono', monospace"/><option value="'Fira Code', monospace"/><option value="Menlo, monospace"/><option value="Monaco, monospace"/></datalist></label><label>Interface text size <span>{Math.round(fontSize / 16 * 100)}%</span><input aria-label="Interface text size" type="range" min="10" max="32" step="1" value={fontSize} onChange={event => setFontSize(Number(event.target.value))}/></label><div className="font-shortcuts">{web ? 'Use this slider for the app’s text; ⌘ / Ctrl + or − zooms the whole page.' : '⌘ / Ctrl + or − resizes all text · 0 resets to 100%'}</div><label>Background opacity <span>{windowTransparency ? `${Math.round(opacity * 100)}%` : 'Host opaque'}</span><input aria-label="Background opacity" disabled={!windowTransparency} type="range" min="0.7" max="1" step="0.01" value={windowTransparency ? opacity : 1} onChange={event => setOpacity(Number(event.target.value))}/></label><p>{windowTransparency ? 'Background layers become transparent. Text and signal cards stay opaque.' : 'This host does not support window transparency. Your opacity preference is retained for supported windows.'}</p>{capabilities?.sourceUrl && <p className="about-line">Flows {capabilities.version} · <ExternalLink href={capabilities.sourceUrl}>Source code</ExternalLink> · AGPL-3.0</p>}</div>}
    {audioPanel && <AudioSettings onClose={() => setAudioPanel(false)}/>}
    {closing && <div className="modal-backdrop"><div className="rename-dialog close-dialog" role="dialog" aria-modal="true" aria-label="Unsaved instrument"><h2>Save changes?</h2><p><strong>{basename(session.document(closing.id)?.path || 'Instrument')}</strong> has unsaved changes.</p><p>Closing without saving discards these changes.</p><div className="dialog-actions"><button className="secondary-button" data-close-action="cancel" disabled={closeBusy} onClick={() => setClosing(null)}>Cancel</button><button className="secondary-button" data-close-action="discard" disabled={closeBusy} onClick={() => void finishClose(closing.group, closing.id, 'discard')}>Discard</button><button autoFocus className="primary-button" data-close-action="save" disabled={closeBusy} onClick={() => void finishClose(closing.group, closing.id, 'save')}>Save</button></div></div></div>}
    {rename && <div className="modal-backdrop" onPointerDown={event => { if (event.target === event.currentTarget) setRename(false); }}><form className="rename-dialog" role="dialog" aria-modal="true" aria-label="Rename instrument" onSubmit={async event => { event.preventDefault(); const result = await perform('document.rename', { name: renameValue.trim() }); if (result !== undefined) setRename(false); }}><header><h2>Rename instrument</h2><button type="button" className="icon-button" title="Cancel rename" onClick={() => setRename(false)}><Icon name="close" size={15}/></button></header><label>Filename<input autoFocus value={renameValue} onChange={event => setRenameValue(event.target.value)} required maxLength={120} pattern="[^/\\\\]+"/></label><footer><button type="button" className="secondary-button" onClick={() => setRename(false)}>Cancel</button><button className="primary-button" disabled={!renameValue.trim()}>Rename</button></footer></form></div>}
    {macrosOpen && <Macros snapshot={snapshot} parameter={macroParameter} onClose={() => setMacrosOpen(false)} onResult={receive} onError={notifyError}/>}
    {expanded && <div className="modal-backdrop" onPointerDown={event => { if (event.target === event.currentTarget) setExpanded(null); }}><div className="signal-modal" role="dialog" aria-modal="true" aria-label={`Inspect ${expanded}`}><div className="modal-title"><div><div className="eyebrow">SIGNAL INSPECTOR</div><h2>{expanded}</h2></div><button className="icon-button" title="Close inspector · Esc" onClick={() => setExpanded(null)}><Icon name="close" size={19}/></button></div><SignalView name={expanded} {...signalProps(expanded)} large/><p>Capture reads the running instrument. Mixed sums audio voices; choose a Voice slot for polyphonic control analysis. Play a note to fill the capture window.</p></div></div>}
  </main>;
}
