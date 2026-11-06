import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import { VisionPanel } from './VisionPanel';

describe('EOS Vision panel state', () => {
  it('explains stopped and stale graph states in both languages', () => {
    const stopped = renderToStaticMarkup(<VisionPanel lang="en" graph={null} active={false} revision="" />);
    const stale = renderToStaticMarkup(<VisionPanel lang="cn" graph={null} active revision="" />);
    expect(stopped).toContain('Run the saved graph to view camera frames.');
    expect(stale).toContain('已保存节点图与运行中节点图不同');
    expect(stale).toContain('role="alert"');
  });
});
