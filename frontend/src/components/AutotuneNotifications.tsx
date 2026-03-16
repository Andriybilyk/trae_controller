import React, { useEffect, useRef } from 'react';
import toast from 'react-hot-toast';
import { useLanguage } from '../contexts/LanguageContext';

type AutoTuneMsg = {
  event: 'autotune_state';
  action?: string;
  fault_active?: boolean;
  fault_reason?: string;
  autotune?: {
    active?: boolean;
    setpoint_c?: number;
    cycles?: number;
    ku?: number;
    pu_s?: number;
    kp?: number;
    ki?: number;
    kd?: number;
  };
};

export const AutotuneNotifications: React.FC = () => {
  const { language } = useLanguage();
  const toastIdRef = useRef<string | undefined>(undefined);
  const lastCyclesRef = useRef<number>(-1);
  const wasActiveRef = useRef<boolean>(false);

  const tr = (ua: string, en: string) => (language === 'ua' ? ua : en);

  useEffect(() => {
    const onWs = (ev: any) => {
      const msg = ev?.detail as AutoTuneMsg | undefined;
      if (!msg || msg.event !== 'autotune_state') return;

      const tune = msg.autotune || {};
      const active = !!tune.active;
      const cycles = typeof tune.cycles === 'number' ? tune.cycles : 0;
      const setpoint = typeof tune.setpoint_c === 'number' ? tune.setpoint_c : undefined;

      const line =
        tr('Автоналаштування', 'Autotune') +
        `: ${cycles}/6` +
        (setpoint !== undefined ? ` @ ${setpoint.toFixed(0)}°C` : '');

      const details =
        ` Ku ${(tune.ku ?? 0).toFixed(2)}` +
        ` Pu ${(tune.pu_s ?? 0).toFixed(0)}s` +
        ` PID ${(tune.kp ?? 0).toFixed(2)}/${(tune.ki ?? 0).toFixed(3)}/${(tune.kd ?? 0).toFixed(2)}`;

      if (msg.action === 'start') {
        wasActiveRef.current = true;
        lastCyclesRef.current = cycles;
        toastIdRef.current = toast.loading(
          `${line}\n${tr('Не залишайте піч без нагляду.', 'Do not leave the kiln unattended.')}`,
          { id: toastIdRef.current }
        );
        return;
      }

      if (active) {
        wasActiveRef.current = true;
        if (cycles !== lastCyclesRef.current) {
          lastCyclesRef.current = cycles;
          toastIdRef.current = toast.loading(`${line}\n${details}`, { id: toastIdRef.current });
        }
        return;
      }

      // inactive
      if (wasActiveRef.current) {
        wasActiveRef.current = false;
        lastCyclesRef.current = -1;
        if (toastIdRef.current) toast.dismiss(toastIdRef.current);

        if (msg.action === 'stop') {
          toast.success(tr('Автоналаштування зупинено', 'Autotune stopped'));
          return;
        }

        if (msg.fault_active) {
          toast.error(
            tr('Автоналаштування не вдалося: ', 'Autotune failed: ') + (msg.fault_reason || '')
          );
          return;
        }

        toast.success(`${tr('Автоналаштування завершено', 'Autotune complete')}\n${details}`);
      }
    };

    window.addEventListener('kiln_ws', onWs as any);
    return () => window.removeEventListener('kiln_ws', onWs as any);
  }, [language]);

  return null;
};

