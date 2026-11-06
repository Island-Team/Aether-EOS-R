import { expect, it } from 'vitest';
import { parseSetting, settingValue } from './NodeGraphManager';

it('round trips selected EGIA numeric settings as JSON numbers', () => {
  expect(parseSetting(settingValue(8, 'positive_integer'), 'positive_integer')).toBe(8);
  expect(parseSetting(settingValue(0.25, 'positive_number'), 'positive_number')).toBe(0.25);
  expect(() => parseSetting('0', 'positive_number')).toThrow();
  expect(() => parseSetting('Infinity', 'positive_number')).toThrow();
});

it('retains owner-managed endpoints as structured display values', () => {
  const endpoint = { host: '127.0.0.1', port: 16060 };
  expect(JSON.parse(settingValue(endpoint, 'endpoint'))).toEqual(endpoint);
});

it('round trips generic setting values without changing their JSON types', () => {
  for (const [type, value] of [['boolean', true], ['integer', -2], ['number', 0.25], ['array', ['Shift', 'Alt']], ['object', { key: 'KeyW' }]] as const) {
    expect(parseSetting(settingValue(value, type), type)).toEqual(value);
  }
  expect(() => parseSetting('{', 'object')).toThrow();
  expect(() => parseSetting('{}', 'array')).toThrow();
});
