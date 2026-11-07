import React, { useMemo, useRef, useState } from 'react';
import type { Language } from '@/types';
import type { NodeOptions, SettingOptionGroup } from '@/services/isos/node-graph';

export function settingOptionGroup(options: NodeOptions | null, nodePath: string, setting: string): SettingOptionGroup | undefined {
  return options?.setting_options?.find((group) => group.node_path === nodePath && group.setting === setting);
}

export function unavailableSelections(options: NodeOptions | null): SettingOptionGroup[] {
  return (options?.setting_options || []).filter((group) => group.selected !== null && !group.options.some((option) => option.value === group.selected && option.available));
}

/** The native list keeps keyboard and screen-reader selection behavior while search narrows its rows. */
export function CatalogSelector({ group, label, value, lang, pin, help, onChange }: { group: SettingOptionGroup; label: string; value: string; lang: Language; pin?: { id: string; version: string }; help?: string; onChange: (value: string) => void }): React.ReactElement {
  const [query, setQuery] = useState('');
  const selectRef = useRef<HTMLSelectElement>(null);
  const selected = group.options.find((option) => option.value === value);
  const rows = useMemo(() => group.options.filter((option) => option.value === value || `${option.value} ${option.label} ${option.description}`.toLowerCase().includes(query.toLowerCase())), [group.options, query, value]);
  const missing = value && !selected;
  const unavailable = missing || (selected && !selected.available);
  const description = selected?.description || '';
  const reason = selected?.reason || (missing ? (lang === 'cn' ? '保存的选项不在当前目录中。请重新选择。' : 'Saved option is absent from the current catalog. Choose another.') : '');
  return <div className="node-graph-catalog-selector">
    {pin && <small>{lang === 'cn' ? '软件包' : 'Package'}: <code>{pin.id}@{pin.version}</code></small>}
    {help && <small>{help}</small>}
    <label>{label} · {lang === 'cn' ? '搜索选项' : 'Search options'}<input type="search" value={query} onChange={(event) => setQuery(event.target.value)} onKeyDown={(event) => { if (event.key === 'ArrowDown' || event.key === 'Enter') { event.preventDefault(); selectRef.current?.focus(); } else if (event.key === 'Escape') setQuery(''); }} /></label>
    <label>{label}<select ref={selectRef} value={value} onChange={(event) => onChange(event.target.value)} aria-invalid={Boolean(unavailable)} aria-describedby={`${group.node_path}-${group.setting}-help`}>
      <option value="">—</option>
      {missing && <option value={value} disabled>{value} · {lang === 'cn' ? '不可用' : 'Unavailable'}</option>}
      {rows.map((option) => <option key={option.value} value={option.value} disabled={!option.available}>{option.label} ({option.value}){option.available ? '' : ` · ${lang === 'cn' ? '不可用' : 'Unavailable'}`}</option>)}
    </select></label>
    <small id={`${group.node_path}-${group.setting}-help`} role={unavailable ? 'alert' : 'status'}>{unavailable ? `${selected?.reason_code || ''} ${reason}` : description}</small>
    <small role="status" aria-live="polite">{rows.length} {lang === 'cn' ? '个选项' : 'options'}</small>
  </div>;
}
