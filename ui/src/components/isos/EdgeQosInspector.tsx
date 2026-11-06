import React, { useEffect, useId, useState } from 'react';
import type { NodeSource, ValidatedEdge } from '@/services/isos/node-graph';
import type { Language } from '@/types';
import { parseFrequency, parsePriority } from './edge-qos';

interface Props {
  source: NodeSource;
  authority?: ValidatedEdge;
  current: boolean;
  readOnly: boolean;
  lang: Language;
  producerHint?: number | null;
  onChange: (change: { priority?: number; frequency_hz?: number | null }) => void;
  onInvalidChange: (invalid: boolean) => void;
}

export function EdgeQosInspector({ source, authority, current, readOnly, lang, producerHint, onChange, onInvalidChange }: Props): React.ReactElement {
  const [priority, setPriority] = useState(String(source.priority ?? 0));
  const [frequency, setFrequency] = useState(source.frequency_hz === undefined ? '' : String(source.frequency_hz));
  const [error, setError] = useState('');
  const help = useId();
  useEffect(() => { setPriority(String(source.priority ?? 0)); setFrequency(source.frequency_hz === undefined ? '' : String(source.frequency_hz)); setError(''); onInvalidChange(false); }, [source.priority, source.frequency_hz]);
  const cn = lang === 'cn';
  const eligible = authority?.routed === true && authority.qos_eligible === true && authority.fixed === false;
  const commitPriority = (): void => {
    const value = parsePriority(priority);
    if (value === null) { setError(cn ? '优先级必须为 -20 到 19 的整数。' : 'Priority must be an integer from -20 to 19.'); onInvalidChange(true); return; }
    setError('');
    onInvalidChange(parseFrequency(frequency) === null);
    if (value !== (source.priority ?? 0)) onChange({ priority: value });
  };
  const commitFrequency = (): void => {
    const value = parseFrequency(frequency);
    if (value === null) { setError(cn ? '频率上限必须为有限正数。' : 'Frequency cap must be a finite positive number.'); onInvalidChange(true); return; }
    setError('');
    onInvalidChange(parsePriority(priority) === null);
    if (value !== source.frequency_hz && !(value === undefined && source.frequency_hz === undefined)) onChange({ frequency_hz: value === undefined ? null : value });
  };
  return <section className="node-graph-edge-qos" aria-label={cn ? '连线服务质量' : 'Edge QoS'}>
    <h3>{cn ? '连线服务质量' : 'Edge QoS'}</h3>
    <p role="status">{authority ? <>{authority.route_class} · {authority.fixed ? (cn ? '固定安全连线' : 'Fixed safety edge') : authority.routed ? (cn ? '已声明路由' : 'Declared route') : (cn ? '仅设计用途' : 'Design only')}</> : (cn ? '检查节点图后显示 IsOS 路由状态。' : 'Check graph to see IsOS route status.')}{authority && !current ? ` · ${cn ? '草稿已更改；保存前重新检查' : 'Draft changed; recheck before saving'}` : ''}</p>
    <p>{cn ? '当前优先级' : 'Effective priority'}: <strong>{source.priority ?? 0}</strong> ({cn ? 'Linux nice，-20 最高；权重 20 − 优先级' : 'Linux nice, -20 highest; weight 20 − priority'}: {20 - (source.priority ?? 0)})</p>
    <p>{cn ? '连线频率上限' : 'Edge frequency cap'}: <strong>{source.frequency_hz === undefined ? (cn ? '无限制' : 'unlimited') : `${source.frequency_hz} Hz`}</strong></p>
    {typeof producerHint === 'number' && <p>{cn ? '生产者预期频率（提示）' : 'Producer expected rate (hint)'}: {producerHint} Hz</p>}
    {!eligible && <p>{authority?.fixed ? (cn ? '固定安全连线不可设置服务质量。' : 'Fixed safety edges cannot set QoS.') : authority ? (cn ? '未路由连线不可设置服务质量。' : 'Unrouted edges cannot set QoS.') : (cn ? '等待 IsOS 检查确认可编辑路由。' : 'Awaiting IsOS validation of an editable route.')}</p>}
    <fieldset disabled={readOnly || !eligible}>
      <legend>{cn ? '选中连线的设置' : 'Selected edge settings'}</legend>
      <label>{cn ? '优先级（Linux nice）' : 'Priority (Linux nice)'}<input type="number" min={-20} max={19} step={1} value={priority} aria-describedby={help} onChange={(event) => { setPriority(event.target.value); onInvalidChange(parsePriority(event.target.value) === null || parseFrequency(frequency) === null); }} onBlur={commitPriority} onKeyDown={(event) => { if (event.key === 'Enter') event.currentTarget.blur(); }} /></label>
      <label>{cn ? '频率上限（Hz；留空为无限制）' : 'Frequency cap (Hz; blank means unlimited)'}<input type="number" min="0" step="any" value={frequency} aria-describedby={help} onChange={(event) => { setFrequency(event.target.value); onInvalidChange(parsePriority(priority) === null || parseFrequency(event.target.value) === null); }} onBlur={commitFrequency} onKeyDown={(event) => { if (event.key === 'Enter') event.currentTarget.blur(); }} /></label>
      <small id={help}>{cn ? '优先级为 -20 至 19 的整数；频率为大于 0 的有限数。保存前 IsOS 会重新检查。' : 'Priority is an integer from -20 to 19; frequency is finite and greater than zero. IsOS rechecks before saving.'}</small>
    </fieldset>
    {error && <p className="node-graph-error" role="alert">{error}</p>}
  </section>;
}
