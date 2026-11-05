import type { AgentActivity, AgentEvent } from './types';
import type { AssistantMessage, Conversation, ConversationSummary, HistoryView } from './protocol.gen';
export type { AssistantMessage, Conversation, ConversationSummary, HistoryView } from './protocol.gen';
export type HistoryFolder = 'active' | 'archived' | 'trash';
export const emptyHistory: HistoryView = { conversations: [], activeConversationId: '', conversation: null };
export const isTerminal = (status: string) => ['done', 'completed', 'failed', 'cancelled', 'canceled', 'interrupted'].includes(status);
export function historyRows(rows: ConversationSummary[], folder: HistoryFolder, query = ''): ConversationSummary[] {
  return rows.filter(row => (folder === 'trash' ? row.deleted : !row.deleted && row.archived === (folder === 'archived')) && row.title.toLowerCase().includes(query.toLowerCase())).sort((a, b) => b.updatedAt - a.updatedAt || a.id.localeCompare(b.id));
}
// An endpoint can finish before the native send acknowledgement returns.
// Working acknowledgements must never replace a terminal reply.
export function mergeAgentEvent(messages: AssistantMessage[], event: AgentEvent): AssistantMessage[] {
  return messages.map(message => {
    if (message.id !== event.requestId || (isTerminal(message.status) && !isTerminal(event.status || ''))) return message;
    let activity = message.activity || [];
    if (event.activity) {
      const previous = activity.find(step => step.id === event.activity!.id);
      // A delayed start event must not turn a completed tool back into a spinner.
      const incoming = previous && previous.state !== 'running' && event.activity.state === 'running' ? previous : event.activity;
      activity = previous ? activity.map(step => step.id === incoming.id ? incoming : step) : [...activity, incoming].slice(-24);
    }
    if (isTerminal(event.status || '')) activity = activity.map(step => step.state === 'running' ? { ...step, state: event.status === 'done' || event.status === 'completed' ? 'done' : event.status === 'cancelled' || event.status === 'canceled' ? 'cancelled' : 'failed' } : step);
    return { ...message,
      // Activity is separate from the answer. Legacy working text becomes a
      // progress cue so acknowledgements/tools never overwrite the reply.
      text: event.error || (event.status !== 'working' ? event.text : undefined) || (event.delta ? message.text + event.delta : message.text),
      activity, startedAt: message.startedAt || (event.status === 'working' ? Date.now() : undefined),
      finishedAt: message.finishedAt || (isTerminal(event.status || '') ? Date.now() : undefined), status: event.status || message.status,
      documentId: event.documentId ?? message.documentId, baseRevision: event.baseRevision ?? message.baseRevision,
      proposedSource: event.proposedSource ?? message.proposedSource, proposalAvailable: event.proposalAvailable ?? message.proposalAvailable };
  });
}
export function mergeLoadedMessages(saved: AssistantMessage[], current: AssistantMessage[]): AssistantMessage[] {
  return saved.map(message => {
    const live = current.find(item => item.id === message.id);
    // A persisted terminal reply is authoritative, especially its proposal flag.
    if (!live) return message;
    if (!isTerminal(message.status)) return live;
    const state: AgentActivity['state'] = message.status === 'done' || message.status === 'completed' ? 'done' : message.status === 'cancelled' || message.status === 'canceled' ? 'cancelled' : 'failed';
    const activity = live.activity?.map(step => step.state === 'running' ? { ...step, state } : step);
    return { ...message, activity, startedAt: live.startedAt, finishedAt: live.finishedAt || (live.startedAt ? Date.now() : undefined) };
  });
}
