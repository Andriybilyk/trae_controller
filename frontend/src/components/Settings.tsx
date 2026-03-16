import React, { useState, useEffect } from 'react';
import { useNavigate } from 'react-router-dom';
import { Save, Zap, Activity, Settings as SettingsIcon, Wrench, Thermometer, Shield, Download, Upload, Monitor, Square } from 'lucide-react';
import { useLanguage } from '../contexts/LanguageContext';
import toast from 'react-hot-toast';
import { API_BASE_URL } from '../config';
import { ConfirmModal } from './ConfirmModal';

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
  const [autotuneInfo, setAutotuneInfo] = useState<any>(null);
  const [pidInfo, setPidInfo] = useState<any>(null);
  const [pidLoading, setPidLoading] = useState(false);
  const [showPidResetConfirm, setShowPidResetConfirm] = useState(false);
  const [showAutotuneStartConfirm, setShowAutotuneStartConfirm] = useState(false);
  const [showAutotuneStopConfirm, setShowAutotuneStopConfirm] = useState(false);
  const [confirmBusy, setConfirmBusy] = useState<null | 'pid_reset' | 'autotune_start' | 'autotune_stop'>(null);

  useEffect(() => {
    // Load from local storage or API
    const saved = localStorage.getItem('kiln_settings');
    if (saved) setSettings(JSON.parse(saved));
  }, []);
  const loadControllerSettings = async () => {
    try {
      const res = await fetch(`${API_BASE_URL}/settings`);
      if (!res.ok) return;
      const d = await res.json();
      if (typeof d.temp_offset_c === 'number') setOffset(d.temp_offset_c);
      if (typeof d.ssrCycles === 'number') setSsrCycles(d.ssrCycles);
      if (typeof d.wattage === 'number') setSettings((prev:any) => ({...prev, wattage: d.wattage}));
      if (typeof d.costPerKwh === 'number') setSettings((prev:any) => ({...prev, costPerKwh: d.costPerKwh}));
      if (typeof d.currency === 'string') setSettings((prev:any) => ({...prev, currency: d.currency}));
      if (typeof d.zones === 'number') setSettings((prev:any) => ({...prev, zones: d.zones}));
    } catch (e) {}
  };
  const loadPid = async () => {
    setPidLoading(true);
    try {
      const res = await fetch(`${API_BASE_URL}/pid`);
      if (res.ok) {
        const d = await res.json();
        setPidInfo(d);
      }
    } catch (e) {}
    setPidLoading(false);
  };

  const resetPid = async () => {
    setShowPidResetConfirm(true);
  };

  const confirmResetPid = async () => {
    try {
      setConfirmBusy('pid_reset');
      const res = await fetch(`${API_BASE_URL}/pid/reset`, { method: 'POST' });
      if (res.ok) {
        await loadPid();
        toast.success("PID reset");
        setShowPidResetConfirm(false);
      } else {
        toast.error("Reset failed");
      }
    } catch (e) {
      toast.error("Connection error to controller.");
    } finally {
      setConfirmBusy(null);
    }
  };

  useEffect(() => {
    loadControllerSettings();

    let alive = true;
    const fetchStatus = async () => {
      try {
        const res = await fetch(`${API_BASE_URL}/status`);
        if (!res.ok) return;
        const d = await res.json();
        if (!alive) return;
        if (d.autotune) setAutotuneInfo(d.autotune);
      } catch (e) {}
    };

    fetchStatus();
    const id = setInterval(fetchStatus, 1000);

    const onWs = (ev: any) => {
      const msg = ev?.detail;
      if (!msg || !msg.event) return;
      if (msg.event === 'settings_changed') { loadControllerSettings(); loadPid(); }
      if (msg.event === 'autotune_state' && msg.autotune) setAutotuneInfo(msg.autotune);
    };
    window.addEventListener('kiln_ws', onWs as any);

    return () => {
      alive = false;
      clearInterval(id);
      window.removeEventListener('kiln_ws', onWs as any);
    };
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
      setShowAutotuneStartConfirm(true);
  };

  const confirmAutoTuneStart = async () => {
      try {
          setConfirmBusy('autotune_start');
          const res = await fetch(`${API_BASE_URL}/autotune`, {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ temp: autotuneTemp })
          });
          const data = await res.json();
          if(res.ok) {
            toast.success(`Autotune started at ${autotuneTemp}°C! Monitor the dashboard.`);
            setShowAutotuneStartConfirm(false);
          } else {
            toast.error("Error: " + (data.error || "Unknown error"));
          }
      } catch (e) {
          toast.error("Connection error to controller.");
      } finally {
          setConfirmBusy(null);
      }
  };

  const handleAutoTuneStop = async () => {
      setShowAutotuneStopConfirm(true);
  };

  const confirmAutoTuneStop = async () => {
      try {
          setConfirmBusy('autotune_stop');
          const res = await fetch(`${API_BASE_URL}/autotune/stop`, { method: 'POST' });
          if(res.ok) {
            toast.success("Autotune stopped");
            setShowAutotuneStopConfirm(false);
          } else {
            toast.error("Stop failed");
          }
      } catch (e) {
          toast.error("Connection error to controller.");
      } finally {
          setConfirmBusy(null);
      }
  };

  return (
    <div className="flex flex-col gap-6 max-w-4xl mx-auto">
      <ConfirmModal
        open={showAutotuneStopConfirm}
        title="Stop Autotune?"
        description="Are you sure you want to stop Autotune now? Heater will turn OFF and results may be incomplete."
        confirmText="STOP Autotune"
        cancelText="Cancel"
        busy={confirmBusy === 'autotune_stop'}
        icon={<Square size={40} fill="currentColor" />}
        onConfirm={confirmAutoTuneStop}
        onCancel={() => setShowAutotuneStopConfirm(false)}
      />

      <ConfirmModal
        open={showAutotuneStartConfirm}
        title="Start Autotune?"
        description={`Start Autotune at ${autotuneTemp}°C? The heater will cycle automatically.`}
        confirmText="Start Autotune"
        cancelText="Cancel"
        variant="danger"
        busy={confirmBusy === 'autotune_start'}
        icon={<Square size={40} fill="currentColor" />}
        onConfirm={confirmAutoTuneStart}
        onCancel={() => setShowAutotuneStartConfirm(false)}
      />

      <ConfirmModal
        open={showPidResetConfirm}
        title="Reset PID to defaults?"
        description="This will overwrite the current PID/offset values. This action cannot be undone."
        confirmText="Reset PID"
        cancelText="Cancel"
        busy={confirmBusy === 'pid_reset'}
        onConfirm={confirmResetPid}
        onCancel={() => setShowPidResetConfirm(false)}
      />

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
                    <button
                        className="w-full sm:w-auto px-6 py-3 bg-red-600 hover:bg-red-500 text-white rounded-lg font-bold transition-colors flex items-center justify-center"
                        onClick={handleAutoTuneStop}
                    >
                        STOP
                    </button>
                </div>
                {autotuneInfo && (
                    <div className="mt-3 text-xs text-zinc-500 font-mono">
                        {(autotuneInfo.active ? 'ACTIVE' : 'IDLE')} • cycles {(autotuneInfo.cycles ?? 0)}/6 • Ku {(autotuneInfo.ku ?? 0).toFixed(2)} • Pu {(autotuneInfo.pu_s ?? 0).toFixed(0)}s • PID {(autotuneInfo.kp ?? 0).toFixed(2)}/{(autotuneInfo.ki ?? 0).toFixed(3)}/{(autotuneInfo.kd ?? 0).toFixed(2)}
                    </div>
                )}
            </div>
        </div>
      </div>

      
      {/* PID / Offset */}
      <div className="bg-kiln-card border border-kiln-border rounded-xl p-6 shadow-lg">
         <h2 className="text-lg font-bold text-white mb-4 flex items-center gap-3 border-b border-zinc-800 pb-4">
            <Wrench size={24} className="text-blue-400" />
            PID / Offset
        </h2>
        <div className="text-sm text-zinc-400">
            {pidLoading && <div>Loading...</div>}
            {!pidLoading && pidInfo && (
                <div className="space-y-2">
                    <div className="font-mono text-zinc-300">Kp {Number(pidInfo.kp).toFixed(2)} • Ki {Number(pidInfo.ki).toFixed(3)} • Kd {Number(pidInfo.kd).toFixed(2)}</div>
                    <div className="font-mono text-zinc-500">Default: {Number(pidInfo.kp_default).toFixed(2)}/{Number(pidInfo.ki_default).toFixed(3)}/{Number(pidInfo.kd_default).toFixed(2)} • Offset {Number(pidInfo.temp_offset_c).toFixed(1)}°C</div>
                    <div className="flex gap-3 pt-2">
                        <button className="px-5 py-2 bg-zinc-800 hover:bg-zinc-700 text-white rounded-lg font-bold transition-colors" onClick={loadPid}>Refresh</button>
                        <button className="px-5 py-2 bg-red-600 hover:bg-red-500 text-white rounded-lg font-bold transition-colors" onClick={resetPid}>Reset to defaults</button>
                    </div>
                </div>
            )}
        </div>
      </div>
      
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
                <p className="text-zinc-500 text-sm mb-3">Preview the native interface for the embedded display (480x320)</p>
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
