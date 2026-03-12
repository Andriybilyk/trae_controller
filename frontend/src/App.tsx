import React from 'react';
import { LanguageProvider } from './contexts/LanguageContext';
import { SchedulesProvider } from './contexts/SchedulesContext';
import AppContent from './AppContent';
import { Toaster } from 'react-hot-toast';

function App() {
  return (
    <LanguageProvider>
      <SchedulesProvider>
        <AppContent />
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
      </SchedulesProvider>
    </LanguageProvider>
  );
}

export default App;
