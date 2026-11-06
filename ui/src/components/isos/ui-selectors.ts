/** UI-owned key choices are physical KeyboardEvent.code values. */
export function validKeyCode(value: unknown): value is string {
  return typeof value === 'string' && /^(?!Unidentified$)[A-Za-z][A-Za-z0-9]{0,31}$/.test(value);
}

export function uiSelectorValueValid(resource: string, value: unknown): boolean {
  return resource === 'keys' && validKeyCode(value);
}
