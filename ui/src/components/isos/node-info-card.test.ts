import { expect, it } from 'vitest';
import { clampInfoCard } from './node-info-card';
it('clamps a floating info card within viewport edges and narrow screens', () => {
  expect(clampInfoCard(900, 900, 340, 340, 800, 600)).toEqual({ left: 452, top: 252 });
  expect(clampInfoCard(-10, -20, 340, 340, 300, 200)).toEqual({ left: 8, top: 8 });
});
