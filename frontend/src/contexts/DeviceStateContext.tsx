import React, { createContext, useContext, useEffect, useMemo, useState } from 'react';
import { getJson } from '../api/http';

export interface DeviceState {
  ts_ms?: number;
  uptime_ms?: number;
  schema_version?: number;
  fw_version?: string;
  temp?: number;
  target?: number;
  output?: number;
  status?: string;
  step?: number;
  totalSteps?: number;
  timeRemaining?: number;
  error?: string;
  schedules_rev?: number;
  settings_rev?: number;
  fan_manual?: boolean;
  fan_auto?: boolean;
  fan_power?: number;
  fan_effective_power?: number;
  fault_active?: boolean;
  fault_code?: number;
  fault_reason?: string;
  wifi_connected?: boolean;
  server_url?: string;
  autotune?: {
    active?: boolean;
    heater_on?: boolean;
    setpoint_c?: number;
    cycles?: number;
    ku?: number;
    pu_s?: number;
    kp?: number;
    ki?: number;
    kd?: number;
  };
  history?: { x: number; y: number }[];
}

interface DeviceStateContextValue {
  state: DeviceState;
  connected: boolean;
}

const DeviceStateContext = createContext<DeviceStateContextValue | undefined>(undefined);

export const DeviceStateProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const [state, setState] = useState<DeviceState>({});
  const [connected, setConnected] = useState(false);

  useEffect(() => {
    let alive = true;

    const apply = (next: any) => {
      if (!alive || !next || typeof next !== 'object') return;
      setState((prev) => ({ ...prev, ...next }));
      if (typeof next.ts_ms === 'number' || typeof next.temp === 'number' || typeof next.status === 'string') {
        setConnected(true);
      }
    };

    const fetchStatus = async () => {
      try {
        const res = await getJson<any>('/status');
        if (res.ok && res.data) apply(res.data);
      } catch {
        setConnected(false);
      }
    };

    fetchStatus();
    const id = setInterval(fetchStatus, 1000);

    const onWsMessage = (ev: any) => {
      apply(ev?.detail);
    };

    const onWsControl = (ev: any) => {
      const msg = ev?.detail;
      if (!msg || typeof msg !== 'object' || !msg.event) return;
      if (msg.event === 'autotune_state' && msg.autotune) {
        apply({ autotune: msg.autotune, fault_active: msg.fault_active, fault_reason: msg.fault_reason });
      }
    };

    window.addEventListener('kiln_ws_message', onWsMessage as any);
    window.addEventListener('kiln_ws', onWsControl as any);

    return () => {
      alive = false;
      clearInterval(id);
      window.removeEventListener('kiln_ws_message', onWsMessage as any);
      window.removeEventListener('kiln_ws', onWsControl as any);
    };
  }, []);

  const value = useMemo(() => ({ state, connected }), [state, connected]);
  return <DeviceStateContext.Provider value={value}>{children}</DeviceStateContext.Provider>;
};

export const useDeviceState = () => {
  const ctx = useContext(DeviceStateContext);
  if (!ctx) throw new Error('useDeviceState must be used within DeviceStateProvider');
  return ctx;
};
