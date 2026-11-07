import React, { useMemo, useState } from 'react';
import type { Language } from '@/types';
import { catalogTypeKey, type NodeTypeDefinition } from '../../services/isos/node-graph';

export function CatalogBrowser({ types, value, lang, onChange }: { types: NodeTypeDefinition[]; value: string; lang: Language; onChange: (id: string) => void }): React.ReactElement {
  const selected = types.find((type) => catalogTypeKey(type) === value);
  const [core, setCore] = useState('');
  const [layer, setLayer] = useState('');
  const [group, setGroup] = useState('');
  const [search, setSearch] = useState('');
  const cores = useMemo(() => [...new Set(types.map((type) => type.core))].sort(), [types]);
  const layers = useMemo(() => [...new Set(types.filter((type) => type.core === core).map((type) => type.layer))].sort(), [types, core]);
  const groups = useMemo(() => [...new Set(types.filter((type) => type.core === core && type.layer === layer).map((type) => type.group))].sort(), [types, core, layer]);
  const visible = types.filter((type) => type.core === core && type.layer === layer && type.group === group && `${type.id} ${type.description || ''}`.toLowerCase().includes(search.toLowerCase()));
  const copy = lang === 'cn' ? { core: '核心', layer: '层', group: '组', type: '节点类型', search: '搜索节点类型', description: '类型描述', fallback: '当前目录没有类型描述（兼容版本）。' } : { core: 'Core', layer: 'Layer', group: 'Group', type: 'Node type', search: 'Search node types', description: 'Type description', fallback: 'No type description in this catalog (compatibility version).' };
  return <div className="node-graph-catalog-browser">
    <label>{copy.core}<select value={core} onChange={(event) => { setCore(event.target.value); setLayer(''); setGroup(''); onChange(''); }}><option value="">—</option>{cores.map((item) => <option key={item} value={item}>{item}</option>)}</select></label>
    <label>{copy.layer}<select value={layer} disabled={!core} onChange={(event) => { setLayer(event.target.value); setGroup(''); onChange(''); }}><option value="">—</option>{layers.map((item) => <option key={item} value={item}>{item}</option>)}</select></label>
    <label>{copy.group}<select value={group} disabled={!layer} onChange={(event) => { setGroup(event.target.value); onChange(''); }}><option value="">—</option>{groups.map((item) => <option key={item} value={item}>{item}</option>)}</select></label>
    <label>{copy.search}<input type="search" value={search} onChange={(event) => setSearch(event.target.value)} /></label>
    <label>{copy.type}<select value={visible.some((type) => catalogTypeKey(type) === value) ? value : ''} disabled={!group} onChange={(event) => onChange(event.target.value)}><option value="">—</option>{visible.map((type) => <option key={catalogTypeKey(type)} value={catalogTypeKey(type)}>{type.id}</option>)}</select></label>
    {selected && <p><strong>{copy.description}:</strong> {selected.description || copy.fallback}</p>}
  </div>;
}
