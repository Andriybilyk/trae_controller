import toast from 'react-hot-toast';
import type { ApiResult } from './http';

export function toastApiError<T>(result: ApiResult<T>, fallback: string, prefix?: string) {
  const message = result.message || fallback;
  toast.error(prefix ? `${prefix}: ${message}` : message);
}

