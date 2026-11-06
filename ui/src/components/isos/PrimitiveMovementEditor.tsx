import React from 'react';
import type { Language } from '@/types';
import type { NodeInstance } from '@/services/isos/node-graph';
import type { Movement } from './eos-control-model';
function asNumber(value: string): number { return value.trim() ? Number(value) : Number.NaN; }
function NumberInput({ label, value, unit, max, onChange }: { label: string; value: number; unit: string; max?: number; onChange: (value: number) => void }): React.ReactElement {
  return <label className="eos-control__field"><span>{label} <small>{unit}</small></span><input type="number" step="any" min={max === undefined ? undefined : -max} max={max} value={Number.isNaN(value) ? '' : value} onChange={(event) => onChange(asNumber(event.target.value))} /></label>;
}

export function MovementEditor({ value, hardware, onChange, lang }: { value: Movement; hardware: NodeInstance; onChange: (value: Movement) => void; lang: Language }): React.ReactElement {
  const limits = hardware.movement?.limits || {};
  if (value.type === 'body_velocity') return <div className="eos-control__fields">
    <NumberInput label="X" unit="m/s" max={limits.max_linear_m_s} value={value.x_m_s} onChange={(n) => onChange({ ...value, x_m_s: n })} />
    <NumberInput label="Y" unit="m/s" max={limits.max_linear_m_s} value={value.y_m_s} onChange={(n) => onChange({ ...value, y_m_s: n })} />
    <NumberInput label="Yaw" unit="rad/s" max={limits.max_angular_rad_s} value={value.yaw_rad_s} onChange={(n) => onChange({ ...value, yaw_rad_s: n })} />
    <NumberInput label={lang === 'cn' ? '有效期' : 'Lease'} unit="ms" max={limits.max_velocity_lease_ms} value={value.valid_for_ms} onChange={(n) => onChange({ ...value, valid_for_ms: n })} />
  </div>;
  if (value.type === 'relative_pose') return <div className="eos-control__fields">
    <NumberInput label="X" unit="m" max={limits.max_translation_m} value={value.x_m} onChange={(n) => onChange({ ...value, x_m: n })} />
    <NumberInput label="Y" unit="m" max={limits.max_translation_m} value={value.y_m} onChange={(n) => onChange({ ...value, y_m: n })} />
    <NumberInput label="Yaw" unit="rad" max={limits.max_rotation_rad} value={value.yaw_rad} onChange={(n) => onChange({ ...value, yaw_rad: n })} />
    <label className="eos-control__field"><span>Frame ID</span><input value={value.frame_id} onChange={(event) => onChange({ ...value, frame_id: event.target.value })} /></label>
  </div>;
  if (value.type === 'joint_positions') return <div className="eos-control__fields">
    {value.joint_names.map((name, index) => <NumberInput key={name} label={name} unit="rad" max={limits.max_joint_position_rad} value={value.joint_positions_rad[index]} onChange={(n) => onChange({ ...value, joint_positions_rad: value.joint_positions_rad.map((position, i) => i === index ? n : position) })} />)}
    <NumberInput label={lang === 'cn' ? '最大速度' : 'Max speed'} unit="rad/s" max={limits.max_joint_speed_rad_s} value={value.max_speed_rad_s ?? Number.NaN} onChange={(n) => onChange({ ...value, max_speed_rad_s: Number.isNaN(n) ? null : n })} />
  </div>;
  const pose = value.pose;
  return <div className="eos-control__fields">
    {pose.position_m.map((position, index) => <NumberInput key={index} label={['X', 'Y', 'Z'][index]} unit="m" max={limits.max_end_effector_translation_m} value={position} onChange={(n) => onChange({ ...value, pose: { ...pose, position_m: pose.position_m.map((old, i) => i === index ? n : old) as [number, number, number] } })} />)}
    {pose.orientation_xyzw.map((orientation, index) => <NumberInput key={index} label={['Qx', 'Qy', 'Qz', 'Qw'][index]} unit="" max={1} value={orientation} onChange={(n) => onChange({ ...value, pose: { ...pose, orientation_xyzw: pose.orientation_xyzw.map((old, i) => i === index ? n : old) as [number, number, number, number] } })} />)}
    <label className="eos-control__field"><span>Frame ID</span><input value={pose.frame_id} onChange={(event) => onChange({ ...value, pose: { ...pose, frame_id: event.target.value } })} /></label>
    <NumberInput label={lang === 'cn' ? '最大速度' : 'Max speed'} unit="m/s" max={limits.max_end_effector_speed_m_s} value={value.max_speed_m_s ?? Number.NaN} onChange={(n) => onChange({ ...value, max_speed_m_s: Number.isNaN(n) ? null : n })} />
  </div>;
}
