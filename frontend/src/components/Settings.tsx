import React, { useState, useEffect } from 'react';
import { useNavigate } from 'react-router-dom';
import { Save, Zap, Activity, Settings as SettingsIcon, Wrench, Thermometer, Shield, Download, Upload, Monitor } from 'lucide-react';
import { useLanguage } from '../contexts/LanguageContext';
import toast from 'react-hot-toast';
import { API_BASE_URL } from '../config';

const Settings = () => {
  const { t } = useLanguage();
  const navigate = useNavigate();
  const [settings, setSettings] = useState({
    wattage: 3.0, // kW
    costPerKwh: 0.15, // $
    currency: '₴',
    zones: 1
  });
  
  const [offset, setOffset] = useState(0);
  const [ssrCycles, setSsrCycles] = useState(15420);
  const [autotuneTemp, setAutotuneTemp] = useState(600);

  useEffect(() => {
    // Load from local storage or API
    const saved = localStorage.getItem('kiln_settings');
    if (saved) setSettings(JSON.parse(saved));
  }, []);

  const handleSave = () => {
    localStorage.setItem('kiln_settings', JSON.stringify(settings));
    fetch(`${API_BASE_URL}/settings`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ...settings, offset, ssrCycles })
    }).then(() => {
        toast.success(t.settings.saved);
    });
  };

  const handleAutoTune = async () => {
      if(confirm(`${t.settings.confirmAutotune} at ${autotuneTemp}°C?`)) {
          try {
              const res = await fetch(`${API_BASE_URL}/autotune`, {
                  method: 'POST',
                  headers: { 'Content-Type': 'application/json' },
                  body: JSON.stringify({ temp: autotuneTemp })
              });
              const data = await res.json();
              if(res.ok) toast.success(`Autotune started at ${autotuneTemp}°C! Monitor the dashboard.`);
              else toast.error("Error: " + (data.error || "Unknown error"));
          } catch (e) {
              toast.error("Connection error to controller.");
          }
      }
  };

  return (
    <div className="flex flex-col gap-6 max-w-4xl mx-auto">
      <div className="flex items-center gap-4 mb-2">
        <div className="p-3 bg-blue-500/10 rounded-xl border border-blue-500/20">
            <SettingsIcon size={32} className="text-blue-500" />
        </div>
        <div>
            <h1 className="text-3xl font-bold text-white tracking-tight">{t.settings.title}</h1>
            <p className="text-slate-400 text-sm">{t.settings.subtitle}</p>
        </div>
      </div>

      {/* General Configuration Section (Energy & Hardware) */}
      <div className="bg-kiln-card border border-kiln-border rounded-xl p-6 shadow-lg">
        <h2 className="text-sm font-bold text-white mb-6 flex items-center gap-3">
            <div className="p-1.5 rounded-lg bg-blue-500/20 text-blue-400">
                <SettingsIcon size={18} />
            </div>
            {t.settings.hardwareConfig}
        </h2>
        
        <div className="space-y-6">
             <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
                <div>
                    <label className="text-xs text-slate-500 font-bold uppercase mb-2 block">{t.settings.kilnPower}</label>
                    <input 
                        className="w-full bg-black border border-zinc-800 rounded-lg p-3 text-white focus:border-blue-500 focus:outline-none transition-colors"
                        type="number" 
                        step="0.1" 
                        value={settings.wattage} 
                        onChange={e => setSettings({...settings, wattage: parseFloat(e.target.value)})} 
                    />
                </div>
                <div>
                    <label className="text-xs text-slate-500 font-bold uppercase mb-2 block">{t.settings.costPerKwh}</label>
                    <input 
                        className="w-full bg-black border border-zinc-800 rounded-lg p-3 text-white focus:border-blue-500 focus:outline-none transition-colors"
                        type="number" 
                        step="0.01" 
                        value={settings.costPerKwh} 
                        onChange={e => setSettings({...settings, costPerKwh: parseFloat(e.target.value)})} 
                    />
                </div>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
                 <div>
                    <label className="text-xs text-slate-500 font-bold uppercase mb-2 block">{t.settings.currency}</label>
                    <select 
                        className="w-full bg-black border border-zinc-800 rounded-lg p-3 text-white focus:border-blue-500 focus:outline-none transition-colors appearance-none"
                        value={settings.currency} 
                        onChange={e => setSettings({...settings, currency: e.target.value})}
                    >
                        <option value="$">USD ($)</option>
                        <option value="€">EUR (€)</option>
                        <option value="₴">UAH (₴)</option>
                        <option value="£">GBP (£)</option>
                    </select>
                </div>
                <div>
                    <label className="text-xs text-slate-500 font-bold uppercase mb-2 block">{t.settings.zoneMode}</label>
                    <select 
                        className="w-full bg-black border border-zinc-800 rounded-lg p-3 text-white focus:border-blue-500 focus:outline-none transition-colors appearance-none"
                        value={settings.zones} 
                        onChange={e => setSettings({...settings, zones: parseInt(e.target.value)})}
                    >
                        <option value="1">{t.settings.zones1}</option>
                        <option value="2">{t.settings.zones2}</option>
                        <option value="3">{t.settings.zones3}</option>
                    </select>
                </div>
            </div>
            
            <button className="w-full py-3 bg-zinc-800 hover:bg-zinc-700 text-white rounded-lg font-medium transition-colors flex items-center justify-center gap-2" onClick={handleSave}>
                <Save size={18} /> {t.settings.saveConfig}
            </button>
        </div>
      </div>

      {/* Maintenance & Diagnostics Section */}
      <div className="bg-kiln-card border border-kiln-border rounded-xl p-6 shadow-lg">
        <h2 className="text-lg font-bold text-white mb-6 flex items-center gap-3 border-b border-zinc-800 pb-4">
            <Wrench size={24} className="text-purple-400" />
            {t.settings.maintenance}
        </h2>
        
        <div className="space-y-8">
            {/* Thermocouple Offset */}
            <div className="flex flex-col sm:flex-row gap-4">
                <div className="p-3 h-12 w-12 rounded-xl bg-zinc-800/50 flex items-center justify-center shrink-0">
                    <Thermometer className="text-yellow-500" size={24} />
                </div>
                <div className="flex-1 w-full overflow-hidden">
                    <h3 className="text-white font-bold mb-1">{t.settings.thermocoupleOffset}</h3>
                    <p className="text-zinc-500 text-sm mb-3">{t.settings.offsetDesc}</p>
                    <div className="flex flex-col sm:flex-row gap-3 w-full">
                        <input 
                            type="number" 
                            value={offset}
                            onChange={(e) => setOffset(Number(e.target.value))}
                            className="bg-black border border-zinc-800 rounded-lg px-3 py-3 text-white w-full sm:w-24 text-center focus:border-purple-500 focus:outline-none font-bold"
                        />
                        <button className="w-full sm:w-auto px-6 py-3 bg-zinc-800 hover:bg-zinc-700 text-white rounded-lg font-medium transition-colors text-sm whitespace-nowrap">
                            {t.settings.save}
                        </button>
                    </div>
                </div>
            </div>

            {/* SSR Relay Cycles */}
            <div className="flex flex-col sm:flex-row gap-4">
                <div className="p-3 h-12 w-12 rounded-xl bg-zinc-800/50 flex items-center justify-center shrink-0">
                    <Activity className="text-blue-400" size={24} />
                </div>
                <div className="flex-1 w-full overflow-hidden">
                    <h3 className="text-white font-bold mb-1">{t.settings.ssrCycles}</h3>
                    <p className="text-zinc-500 text-sm mb-3">{t.settings.ssrDesc}</p>
                    <div className="flex flex-col sm:flex-row sm:items-center gap-3 w-full">
                        <div className="flex items-center bg-black border border-zinc-800 rounded-lg w-full sm:w-fit overflow-hidden relative">
                            <input 
                                type="number"
                                className="px-3 py-3 bg-transparent text-white font-mono font-bold text-lg w-full sm:w-28 focus:outline-none text-left pl-3 pr-16"
                                value={ssrCycles}
                                onChange={(e) => setSsrCycles(parseInt(e.target.value) || 0)}
                            />
                            <div className="absolute right-0 top-0 bottom-0 flex items-center px-3 text-zinc-500 text-sm border-l border-zinc-800 bg-zinc-900/50 pointer-events-none">{t.settings.cycles}</div>
                        </div>
                        <button className="w-full sm:w-auto px-6 py-3 bg-zinc-800 hover:bg-zinc-700 text-white rounded-lg font-medium transition-colors text-sm whitespace-nowrap" onClick={handleSave}>
                            {t.settings.save}
                        </button>
                    </div>
                </div>
            </div>

            {/* Element Health */}
            <div className="flex gap-4">
                <div className="p-3 h-12 w-12 rounded-xl bg-zinc-800/50 flex items-center justify-center shrink-0">
                    <Zap className="text-yellow-400" size={24} />
                </div>
                <div className="flex-1">
                    <h3 className="text-white font-bold mb-1">{t.settings.elementHealth}</h3>
                    <p className="text-zinc-500 text-sm mb-3">{t.settings.elementDesc}</p>
                    <div className="flex items-center gap-3">
                        <div className="flex-1 h-4 bg-zinc-800 rounded-full overflow-hidden">
                            <div className="h-full bg-emerald-500 w-[85%] rounded-full"></div>
                        </div>
                        <span className="text-zinc-500 text-xs font-bold">85%</span>
                    </div>
                </div>
            </div>
        </div>
      </div>

      {/* Autotune Section (Separate Card) */}
      <div className="bg-kiln-card border border-kiln-border rounded-xl p-6 shadow-lg">
         <h2 className="text-lg font-bold text-white mb-6 flex items-center gap-3 border-b border-zinc-800 pb-4">
            <Activity size={24} className="text-purple-400" />
            {t.settings.serviceAutotune}
        </h2>
        <div className="flex flex-col md:flex-row gap-6">
            <div className="p-3 h-12 w-12 rounded-xl bg-purple-500/10 flex items-center justify-center shrink-0 border border-purple-500/20">
                <Activity className="text-purple-400" size={24} />
            </div>
            <div className="flex-1">
                <p className="text-zinc-400 text-sm mb-4">{t.settings.warningText}</p>
                <div className="flex flex-col sm:flex-row gap-3">
                    <div className="relative w-full sm:w-48">
                        <input 
                            type="number"
                            value={autotuneTemp}
                            onChange={(e) => setAutotuneTemp(Number(e.target.value))}
                            placeholder="e.g., 600"
                            className="w-full bg-black border border-zinc-800 rounded-lg p-3 text-white focus:border-purple-500 focus:outline-none transition-colors pr-12"
                        />
                        <span className="absolute right-3 top-1/2 -translate-y-1/2 text-zinc-500">°C</span>
                    </div>
                    <button 
                        className="w-full sm:w-auto px-6 py-3 bg-purple-600 hover:bg-purple-500 text-white rounded-lg font-bold transition-colors shadow-lg shadow-purple-900/20 flex items-center justify-center gap-2"
                        onClick={handleAutoTune}
                    >
                        <Activity size={18} /> {t.settings.initiateCalib}
                    </button>
                </div>
            </div>
        </div>
      </div>

      {/* Safety Systems & Data Logging Section */}
      <div className="bg-kiln-card border border-kiln-border rounded-xl p-6 shadow-lg">
         <h2 className="text-lg font-bold text-white mb-6 flex items-center gap-3 border-b border-zinc-800 pb-4">
            <Shield size={24} className="text-red-400" />
            {t.settings.safety}
        </h2>

        <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
            {/* Data Logging */}
            <div className="bg-black/20 border border-zinc-800 rounded-xl p-5">
                <h3 className="text-white font-bold mb-2">{t.settings.dataLog}</h3>
                <p className="text-zinc-500 text-sm mb-4 h-10">{t.settings.dataLogDesc}</p>
                <button className="w-full py-3 bg-zinc-800 hover:bg-zinc-700 text-white rounded-lg font-medium transition-colors flex items-center justify-center gap-2">
                    <Download size={18} /> {t.settings.downloadCsv}
                </button>
            </div>

            {/* Firmware Update */}
            <div className="bg-black/20 border border-zinc-800 rounded-xl p-5">
                <h3 className="text-white font-bold mb-2">{t.settings.firmware}</h3>
                <p className="text-zinc-500 text-sm mb-4 h-10">{t.settings.firmwareDesc}</p>
                <button className="w-full py-3 bg-blue-600 hover:bg-blue-500 text-white rounded-lg font-medium transition-colors flex items-center justify-center gap-2 shadow-lg shadow-blue-900/20">
                    <Upload size={18} /> {t.settings.uploadBin}
                </button>
            </div>
        </div>
      </div>
      {/* Developer Section */}
      <div className="bg-kiln-card border border-kiln-border rounded-xl p-6 shadow-lg mb-8">
         <h2 className="text-lg font-bold text-white mb-6 flex items-center gap-3 border-b border-zinc-800 pb-4">
            <Monitor size={24} className="text-emerald-400" />
            Developer Tools
        </h2>
        
        <div className="flex gap-4 items-center">
            <div className="p-3 h-12 w-12 rounded-xl bg-emerald-500/10 flex items-center justify-center shrink-0 border border-emerald-500/20">
                <Monitor className="text-emerald-400" size={24} />
            </div>
            <div className="flex-1">
                <h3 className="text-white font-bold mb-1">Controller Screen Simulator</h3>
                <p className="text-zinc-500 text-sm mb-3">Preview the native interface for Waveshare 4.3" ESP32-S3 (800x480)</p>
                <button 
                    onClick={() => navigate('/controller-sim')}
                    className="px-6 py-2 bg-zinc-800 hover:bg-zinc-700 text-white rounded-lg font-medium transition-colors border border-zinc-700"
                >
                    Launch Simulator
                </button>
            </div>
        </div>
      </div>

    </div>
  );
};

export default Settings;
