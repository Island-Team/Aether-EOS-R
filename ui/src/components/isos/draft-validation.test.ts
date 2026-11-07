import { describe, expect, it } from 'vitest';
import { draftValidationStamp, validationDisplayIsCurrent, validationResponseIsCurrent } from './draft-validation';

describe('draft validation generation', () => {
  it('ignores a late response after edit, undo, or redo and accepts the newly checked draft', () => {
    const original = { nodes: ['camera'] };
    const edited = { nodes: ['camera', 'gate'] };
    const checkedOriginal = draftValidationStamp(original, 7);
    expect(validationDisplayIsCurrent(checkedOriginal, original, 7)).toBe(true);
    expect(validationResponseIsCurrent(7, 8)).toBe(false); // edit
    expect(validationDisplayIsCurrent(checkedOriginal, edited, 8)).toBe(false);
    expect(validationResponseIsCurrent(7, 9)).toBe(false); // undo
    expect(validationDisplayIsCurrent(checkedOriginal, original, 9)).toBe(false);
    expect(validationResponseIsCurrent(7, 10)).toBe(false); // redo
    expect(validationDisplayIsCurrent(checkedOriginal, edited, 10)).toBe(false);
    const checkedEdited = draftValidationStamp(edited, 10);
    expect(validationDisplayIsCurrent(checkedEdited, edited, 10)).toBe(true);
  });
});
