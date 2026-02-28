import React from 'react';
import { Routes, Route, NavLink, Outlet, useLocation } from 'react-router-dom';
import { LayoutDashboard, Calendar, Flame, Settings as SettingsIcon, Globe, ShieldAlert, Activity, History as HistoryIcon } from 'lucide-react';
import Dashboard from './components/Dashboard';
import ScheduleEditor from './components/ScheduleEditor';
import GlassWizard from './components/GlassWizard';
import Settings from './components/Settings';
import History from './components/History'; // Import History component
import ControllerScreen from './components/ControllerScreen';
import { useLanguage } from './contexts/LanguageContext';
import toast from 'react-hot-toast';
import { API_BASE_URL } from './config';

// Layout Component
const MainLayout = () => {
  const { language, setLanguage, t } = useLanguage();
  
  const navLinkClass = ({ isActive }: { isActive: boolean }) =>
    `flex items-center gap-2 px-3 py-1.5 rounded-md text-sm font-medium transition-colors ${
      isActive 
        ? 'bg-zinc-800 text-white shadow-sm' 
        : 'text-zinc-400 hover:text-white hover:bg-zinc-800/50'
    }`;

  const mobileNavLinkClass = ({ isActive }: { isActive: boolean }) =>
    `flex flex-col items-center justify-center w-full h-full gap-1 text-xs font-medium transition-colors ${
      isActive 
        ? 'text-kiln-accent' 
        : 'text-zinc-500 hover:text-zinc-300'
    }`;

  const toggleLanguage = () => {
    setLanguage(language === 'en' ? 'ua' : 'en');
  };

  const handleEmergencyStop = async () => {
      try {
          await fetch(`${API_BASE_URL}/stop`, { method: 'POST' });
          toast.error("EMERGENCY STOP TRIGGERED!", {
              duration: 5000,
              style: {
                  background: '#ef4444',
                  color: 'white',
                  fontWeight: 'bold',
                  fontSize: '1.2rem'
              },
              icon: '🛑'
          });
      } catch (e) {
          toast.error("Failed to send stop command!");
      }
  };

  return (
    <div className="flex flex-col h-screen w-screen bg-kiln-bg text-kiln-text overflow-hidden font-sans selection:bg-kiln-accent selection:text-black">
      {/* Navbar */}
      <nav className="h-16 border-b border-kiln-border flex items-center justify-between px-6 bg-kiln-bg z-50 shrink-0">
        {/* Logo */}
        <div className="flex items-center gap-3 select-none">
          <div className="bg-kiln-accent p-1.5 rounded-lg shadow-[0_0_15px_rgba(16,185,129,0.3)]">
            <Flame className="text-black fill-black" size={20} />
          </div>
          <span className="text-xl font-bold tracking-tight text-white">KilnPro</span>
        </div>

        {/* Main Nav (Desktop) */}
        <div className="hidden md:flex items-center gap-1 bg-kiln-card p-1 rounded-lg border border-kiln-border">
          <NavLink to="/" className={navLinkClass}>
            <LayoutDashboard size={16} /> <span>{t.nav.dashboard}</span>
          </NavLink>
          <NavLink to="/schedules" className={navLinkClass}>
            <Calendar size={16} /> <span>{t.nav.schedules}</span>
          </NavLink>
          <NavLink to="/history" className={navLinkClass}>
            <HistoryIcon size={16} /> <span>{t.nav.history}</span>
          </NavLink>
          <NavLink to="/glass" className={navLinkClass}>
             <Flame size={16} /> <span>{t.nav.glassWizard}</span>
          </NavLink>
          <NavLink to="/settings" className={navLinkClass}>
            <SettingsIcon size={16} /> <span>{t.nav.settings}</span>
          </NavLink>
        </div>

        {/* Right Actions */}
        <div className="flex items-center gap-2 md:gap-4 text-sm text-zinc-500 font-medium select-none">
          
          {/* E-STOP BUTTON */}
          <button 
            onClick={handleEmergencyStop}
            className="flex items-center gap-2 bg-red-600 hover:bg-red-700 text-white px-3 py-1.5 md:px-4 md:py-2 rounded-md font-bold transition-all shadow-[0_0_15px_rgba(220,38,38,0.5)] active:scale-95 text-xs md:text-sm"
          >
            <ShieldAlert size={16} /> STOP
          </button>

          <div className="h-6 w-px bg-zinc-800 mx-1 md:mx-2"></div>

          <button 
            onClick={toggleLanguage}
            className="flex items-center gap-2 hover:text-white transition-colors uppercase"
          >
            <Globe size={16} /> <span className="hidden md:inline">{language}</span>
          </button>
        </div>
      </nav>

      {/* Main Content */}
      <main className="flex-1 overflow-y-auto overflow-x-hidden relative p-4 md:p-6 pb-20 md:pb-6">
        <Outlet />
      </main>

      {/* Mobile Bottom Nav */}
      <div className="md:hidden fixed bottom-0 left-0 right-0 h-16 bg-zinc-900 border-t border-zinc-800 flex justify-around items-center z-50 px-2 pb-safe">
        <NavLink to="/" className={mobileNavLinkClass}>
            <LayoutDashboard size={20} /> <span className="text-[10px]">{t.nav.dashboard}</span>
        </NavLink>
        <NavLink to="/schedules" className={mobileNavLinkClass}>
            <Calendar size={20} /> <span className="text-[10px]">{t.nav.schedules}</span>
        </NavLink>
        <NavLink to="/history" className={mobileNavLinkClass}>
            <HistoryIcon size={20} /> <span className="text-[10px]">{t.nav.history}</span>
        </NavLink>
        <NavLink to="/glass" className={mobileNavLinkClass}>
            <Flame size={20} /> <span className="text-[10px]">{t.nav.glassWizard}</span>
        </NavLink>
        <NavLink to="/settings" className={mobileNavLinkClass}>
            <SettingsIcon size={20} /> <span className="text-[10px]">{t.nav.settings}</span>
        </NavLink>
      </div>
    </div>
  );
};

function AppContent() {
  return (
    <Routes>
      <Route element={<MainLayout />}>
        <Route path="/" element={<Dashboard />} />
        <Route path="/schedules" element={<ScheduleEditor />} />
        <Route path="/history" element={<History />} />
        <Route path="/glass" element={<GlassWizard />} />
        <Route path="/settings" element={<Settings />} />
      </Route>
      <Route path="/controller-sim" element={<ControllerScreen />} />
    </Routes>
  );
}

export default AppContent;
