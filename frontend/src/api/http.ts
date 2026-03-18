import { API_BASE_URL } from '../config';

export interface ApiResult<T> {
  ok: boolean;
  status: number;
  code: string;
  message: string;
  data: T | null;
}

function normalizeResult<T>(status: number, httpOk: boolean, raw: any): ApiResult<T> {
  const code =
    (typeof raw?.code === 'string' && raw.code) ||
    (typeof raw?.error === 'string' && raw.error) ||
    (httpOk ? 'ok' : 'http_error');
  const message =
    (typeof raw?.message === 'string' && raw.message) ||
    (typeof raw?.error === 'string' && raw.error) ||
    (typeof raw?.status === 'string' && raw.status) ||
    (httpOk ? 'ok' : 'Request failed');
  const ok = typeof raw?.ok === 'boolean' ? raw.ok : (httpOk && !raw?.error);
  return {
    ok,
    status,
    code,
    message,
    data: (raw ?? null) as T | null
  };
}

export async function requestJson<T>(path: string, init?: RequestInit): Promise<ApiResult<T>> {
  try {
    const res = await fetch(`${API_BASE_URL}${path}`, init);
    const data = await res.json().catch(() => ({}));
    return normalizeResult<T>(res.status, res.ok, data);
  } catch {
    return {
      ok: false,
      status: 0,
      code: 'network_error',
      message: 'Connection error',
      data: null
    };
  }
}

export const getJson = <T>(path: string) => requestJson<T>(path);
export const postJson = <T>(path: string, body?: unknown) =>
  requestJson<T>(path, {
    method: 'POST',
    headers: body !== undefined ? { 'Content-Type': 'application/json' } : undefined,
    body: body !== undefined ? JSON.stringify(body) : undefined
  });
export const deleteJson = <T>(path: string) =>
  requestJson<T>(path, { method: 'DELETE' });

