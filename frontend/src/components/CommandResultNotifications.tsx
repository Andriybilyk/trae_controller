import React, { useEffect, useRef } from 'react';
import toast from 'react-hot-toast';
import { useLanguage } from '../contexts/LanguageContext';

type CommandResultMsg = {
  event: 'command_result';
  rev?: number;
  action?: string;
  ok?: boolean;
  code?: string;
  message?: string;
  source?: string;
};

export const CommandResultNotifications: React.FC = () => {
  const { language } = useLanguage();
  const lastRevRef = useRef<number>(0);

  const tr = (ua: string, en: string) => (language === 'ua' ? ua : en);

  useEffect(() => {
    const onWs = (ev: any) => {
      const msg = ev?.detail as CommandResultMsg | undefined;
      if (!msg || msg.event !== 'command_result') return;

      const rev = typeof msg.rev === 'number' ? msg.rev : 0;
      if (rev > 0 && rev <= lastRevRef.current) return;
      if (rev > 0) lastRevRef.current = rev;

      const action = msg.action || 'command';
      const payload = msg.message || msg.code || (msg.ok ? 'ok' : 'error');
      const prefix = tr('Команда', 'Command');
      const text = `${prefix} ${action}: ${payload}`;

      if (msg.ok) toast.success(text);
      else toast.error(text);
    };

    window.addEventListener('kiln_ws', onWs as any);
    return () => window.removeEventListener('kiln_ws', onWs as any);
  }, [language]);

  return null;
};
