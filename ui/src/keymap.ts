// Editor shortcuts. Browsers reserve ⌘/Ctrl+W, ⌘/Ctrl+1–9 and Ctrl+Tab for
// their own tabs, so the web app moves those actions to Option/Alt.
export type EditorAction = 'close' | 'split' | 'focus-first' | 'focus-second' | 'next-tab' | 'previous-tab';
export interface KeyLike { key: string; code: string; metaKey: boolean; ctrlKey: boolean; altKey: boolean; shiftKey: boolean; isComposing?: boolean; keyCode?: number }

export function editorShortcut(event: KeyLike, { mac, web }: { mac: boolean; web: boolean }): EditorAction | undefined {
  if (event.isComposing || event.keyCode === 229) return undefined;
  const mod = mac ? event.metaKey && !event.ctrlKey : event.ctrlKey && !event.metaKey;
  if (mod && !event.altKey && !event.shiftKey && event.code === 'Backslash') return 'split';
  if (web) {
    // Option/Alt: no browser binds these, and event.code ignores the
    // characters macOS produces for Option combinations.
    // Shift is left alone: Option+Shift types characters such as ’ and ”.
    if (!event.altKey || event.metaKey || event.ctrlKey || event.shiftKey) return undefined;
    if (event.code === 'KeyW') return 'close';
    if (event.code === 'Digit1') return 'focus-first';
    if (event.code === 'Digit2') return 'focus-second';
    if (event.code === 'BracketRight' || event.key === 'PageDown') return 'next-tab';
    if (event.code === 'BracketLeft' || event.key === 'PageUp') return 'previous-tab';
    return undefined;
  }
  if (event.altKey) return undefined;
  if (mod && !event.shiftKey && event.code === 'KeyW') return 'close';
  if (mod && !event.shiftKey && event.code === 'Digit1') return 'focus-first';
  if (mod && !event.shiftKey && event.code === 'Digit2') return 'focus-second';
  if (mod && event.key === 'PageDown') return 'next-tab';
  if (mod && event.key === 'PageUp') return 'previous-tab';
  if (event.ctrlKey && !event.metaKey && event.key === 'Tab') return event.shiftKey ? 'previous-tab' : 'next-tab';
  return undefined;
}

/** Text size shortcuts (⌘/Ctrl + − 0) stay with the browser's zoom on the web. */
export function textSizeShortcut(event: KeyLike, { web }: { web: boolean }): 'larger' | 'smaller' | 'reset' | undefined {
  if (web || !(event.metaKey || event.ctrlKey) || event.altKey || event.isComposing) return undefined;
  if (event.key === '=' || event.key === '+') return 'larger';
  if (event.key === '-') return 'smaller';
  if (event.key === '0') return 'reset';
  return undefined;
}

/** Human labels for menus and tooltips. */
export function shortcutLabel(action: EditorAction, { mac, web }: { mac: boolean; web: boolean }): string {
  const mod = mac ? '⌘' : 'Ctrl+', alt = mac ? '⌥' : 'Alt+';
  const labels: Record<EditorAction, [string, string]> = {
    close: [`${mod}W`, `${alt}W`], split: [`${mod}\\`, `${mod}\\`],
    'focus-first': [`${mod}1`, `${alt}1`], 'focus-second': [`${mod}2`, `${alt}2`],
    'next-tab': ['Ctrl+Tab', `${alt}]`], 'previous-tab': ['Ctrl+Shift+Tab', `${alt}[`],
  };
  return labels[action][web ? 1 : 0];
}
