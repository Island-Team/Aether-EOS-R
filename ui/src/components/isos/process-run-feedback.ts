import type { ProcessStatus } from '@/services/isos/node-graph';
import type { Language } from '@/types';

export function processRunFeedback(status: ProcessStatus, operation: 'run' | 'restart', lang: Language): string {
  if (status.last_error) return status.last_error;
  if (status.state === 'degraded') return lang === 'cn' ? '进程状态异常。' : 'Process state degraded.';
  return operation === 'run'
    ? lang === 'cn' ? '已启动当前节点图。' : 'Current graph started.'
    : lang === 'cn' ? '已用当前节点图重启。' : 'Restarted with current graph.';
}
