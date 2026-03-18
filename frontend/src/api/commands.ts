import { postJson } from './http';

export interface CommandResponse {
  ok: boolean;
  code: string;
  message: string;
}

export async function postCommand(path: string, body?: unknown): Promise<CommandResponse> {
  const res = await postJson<any>(path, body);
  return { ok: res.ok, code: res.code, message: res.message };
}
