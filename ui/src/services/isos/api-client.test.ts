import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

beforeEach(() => {
  vi.resetModules();
  vi.stubGlobal('ISOSUI_API_ORIGIN', 'http://127.0.0.1:16000');
});

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('IsOS API client', () => {
  it('uses the current node graph endpoint and sends a loopback request', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, status: 200, json: async () => ({ schema_version: '0.1.0', ok: true, data: { revision: 'saved' }, error: null }) });
    vi.stubGlobal('fetch', fetchMock);
    const { request } = await import('./api-client');
    expect(await request('/graph')).toEqual({ revision: 'saved' });
    expect(fetchMock).toHaveBeenCalledWith('http://127.0.0.1:16000/api/isos/v1/graph', expect.objectContaining({ method: 'GET', credentials: 'omit', targetAddressSpace: 'loopback' }));
  });

  it('preserves revision conflict guidance', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: false, status: 409, json: async () => ({ schema_version: '0.1.0', ok: false, data: null, error: { code: 'revision_conflict', message: 'changed', details: null, correlation_id: 'req-1' } }) }));
    const { request } = await import('./api-client');
    await expect(request('/graph', 'PUT', { revision: 'old' })).rejects.toMatchObject({ name: 'IsosApiError', status: 409, message: expect.stringContaining('Refresh before applying') });
  });

  it('rejects a mismatched API envelope', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: true, status: 200, json: async () => ({ schema_version: 2, ok: true, data: {}, error: null }) }));
    const { request } = await import('./api-client');
    await expect(request('/graph')).rejects.toThrow('needs the IsOS v1 API');
  });
});
