import React from 'react';
import { Routes, Route, NavLink } from 'react-router-dom';
import { LayoutDashboard, Calendar, Flame, Settings as SettingsIcon, Globe } from 'lucide-react';
import Dashboard from './components/Dashboard';
import ScheduleEditor from './components/ScheduleEditor';
import GlassWizard from './components/GlassWizard';
import Settings from './components/Settings';
import { useLanguage } from './contexts/LanguageContext';

function AppContent() {
  const { language, setLanguage, t } = useLanguage();

  const navLinkClass = ({ isActive }: { isActive: boolean }) =>
    `flex items-center gap-2 px-3 py-1.5 rounded-md text-sm font-medium transition-colors ${
      isActive 
        ? 'bg-zinc-800 text-white shadow-sm' 
        : 'text-zinc-400 hover:text-white hover:bg-zinc-800/50'
    }`;

  const toggleLanguage = () => {
    setLanguage(language === 'en' ? 'ua' : 'en');
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

        {/* Main Nav */}
        <div className="flex items-center gap-1 bg-kiln-card p-1 rounded-lg border border-kiln-border">
          <NavLink to="/" className={navLinkClass}>
            <LayoutDashboard size={16} /> <span>{t.nav.dashboard}</span>
          </NavLink>
          <NavLink to="/schedules" className={navLinkClass}>
            <Calendar size={16} /> <span>{t.nav.schedules}</span>
          </NavLink>
          <NavLink to="/glass" className={navLinkClass}>
             <Flame size={16} /> <span>{t.nav.glassWizard}</span>
          </NavLink>
          <NavLink to="/settings" className={navLinkClass}>
            <SettingsIcon size={16} /> <span>{t.nav.settings}</span>
          </NavLink>
        </div>

        {/* Right Actions */}
        <div className="flex items-center gap-6 text-sm text-zinc-500 font-medium select-none">
          <button 
            onClick={toggleLanguage}
            className="flex items-center gap-2 hover:text-white transition-colors uppercase"
          >
            <Globe size={16} /> <span>{language}</span>
          </button>
        </div>
      </nav>

      {/* Main Content */}
      <main className="flex-1 overflow-y-auto overflow-x-hidden relative p-6">
        <Routes>
          <Route path="/" element={<Dashboard />} />
          <Route path="/schedules" element={<ScheduleEditor />} />
          <Route path="/glass" element={<GlassWizard />} />
          <Route path="/settings" element={<Settings />} />
        </Routes>
      </main>
    </div>
  );
}

export default AppContent;
