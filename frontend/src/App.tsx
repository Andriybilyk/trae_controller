import React from 'react';
import { LanguageProvider } from './contexts/LanguageContext';
import { SchedulesProvider } from './contexts/SchedulesContext';
import { DeviceStateProvider } from './contexts/DeviceStateContext';
import AppContent from './AppContent';
import { Toaster } from 'react-hot-toast';
import { AutotuneNotifications } from './components/AutotuneNotifications';
import { CommandResultNotifications } from './components/CommandResultNotifications';

function App() {
  return (
    <LanguageProvider>
      <SchedulesProvider>
        <DeviceStateProvider>
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
