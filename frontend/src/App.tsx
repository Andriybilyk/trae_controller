import React, { useEffect, useState } from 'react';
import { LanguageProvider } from './contexts/LanguageContext';
import { SchedulesProvider } from './contexts/SchedulesContext';
import { DeviceStateProvider } from './contexts/DeviceStateContext';
import AppContent from './AppContent';
import { Toaster } from 'react-hot-toast';
import { AutotuneNotifications } from './components/AutotuneNotifications';
import { CommandResultNotifications } from './components/CommandResultNotifications';
import { StartupSplash } from './components/StartupSplash';
import { getJson } from './api/http';

function App() {
  const [showSplash, setShowSplash] = useState(true);

  useEffect(() => {
    let mounted = true;
    const minDelay = new Promise(resolve => setTimeout(resolve, 1200));
    const maxDelay = new Promise(resolve => setTimeout(resolve, 4500));
    const statusReady = getJson<any>('/status').catch(() => null);

    Promise.race([
      Promise.all([minDelay, statusReady]),
      maxDelay
    ]).finally(() => {
      if (mounted) setShowSplash(false);
    });

    return () => {
      mounted = false;
    };
  }, []);

  return (
    <LanguageProvider>
      <SchedulesProvider>
        <DeviceStateProvider>
          {showSplash && <StartupSplash />}
          <AppContent />
          <AutotuneNotifications />
          <CommandResultNotifications />
          <Toaster 
            position="top-center"
            toastOptions={{
              style: {
                background: '#18181b',
                color: '#fff',
                border: '1px solid #27272a',
              },
              success: {
                iconTheme: {
                  primary: '#10b981',
                  secondary: 'black',
                },
              },
              error: {
                iconTheme: {
                  primary: '#ef4444',
                  secondary: 'black',
                },
              },
            }}
          />
        </DeviceStateProvider>
      </SchedulesProvider>
    </LanguageProvider>
  );
}

export default App;
