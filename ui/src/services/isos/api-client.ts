/** Versioned API client for the local IsOS process. */
interface ApiFailure { code: string; message: string; details: unknown; correlation_id: string }
interface Envelope<T> { schema_version: '0.1.0'; ok: boolean; data: T | null; error: ApiFailure | null }
type LoopbackRequestInit = RequestInit & { targetAddressSpace?: 'loopback' };

export class IsosApiError extends Error {
  constructor(message: string, public readonly status: number, public readonly code?: string) {
    super(message);
    this.name = 'IsosApiError';
  }
}

function diagnosticCode(data: unknown): string | undefined {
  if (!data || typeof data !== 'object' || !('diagnostics' in data) || !Array.isArray(data.diagnostics)) return undefined;
  const first: unknown = data.diagnostics[0];
  return first && typeof first === 'object' && 'code' in first && typeof first.code === 'string' ? first.code : undefined;
}

export async function request<T>(path: string, method = 'GET', body?: unknown, signal?: AbortSignal): Promise<T> {
  let response: Response;
  try {
    const options: LoopbackRequestInit = {
      method,
      credentials: 'omit',
      cache: 'no-store',
      signal,
      headers: body === undefined ? {} : { 'Content-Type': 'application/json' },
      ...(body === undefined ? {} : { body: JSON.stringify(body) }),
      // Chrome uses this hint for HTTPS-to-HTTP loopback permission and address verification.
      mode: 'cors',
      targetAddressSpace: 'loopback',
    };
    response = await fetch(`${ISOSUI_API_ORIGIN}/api/isos/v1${path}`, options);
  } catch {
    throw new Error(`Cannot reach IsOS at ${ISOSUI_API_ORIGIN}. Run isos serve and check the service address and port.`);
  }
  let envelope: Envelope<T> | null = null;
  try { envelope = await response.json() as Envelope<T>; } catch { /* HTTP failures can be plain text. */ }
  if (response.ok && !envelope) {
    throw new Error('The IsOS API address returned a page instead of API data. Check the API port, then run isos serve.');
  }
  if (envelope && envelope.schema_version !== '0.1.0') {
    throw new Error('This IsosUI build needs the IsOS v1 API. Update IsOS or use a compatible UI release.');
  }
  if (!response.ok || !envelope?.ok || envelope.data === null) {
    const code = diagnosticCode(envelope?.error?.details) || envelope?.error?.code;
    if (response.status === 409) throw new IsosApiError('This graph changed in another editor. Refresh before applying.', 409, code);
    throw new IsosApiError(envelope?.error?.message || `IsOS request failed (${response.status}).`, response.status, code);
  }
  return envelope.data;
}
