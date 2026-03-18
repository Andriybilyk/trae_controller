import React, { useState, useEffect } from 'react';
import { Save, Zap, Activity, Settings as SettingsIcon, Wrench, Thermometer, Shield, Download, Upload, Square, Globe } from 'lucide-react';
import { useLanguage } from '../contexts/LanguageContext';
import { useDeviceState } from '../contexts/DeviceStateContext';
import toast from 'react-hot-toast';
import { getJson, postJson, requestJson } from '../api/http';
import { toastApiError } from '../api/notify';
import { ConfirmModal } from './ConfirmModal';

const Settings = () => {
  const { t } = useLanguage();
  const [settings, setSettings] = useState({
    wattage: 3.0, // kW
    maxC: 1300,
    costPerKwh: 0.15, // $
    currency: '₴',
    zones: 1
  });
  
  const [offset, setOffset] = useState(0);
  const [ssrCycles, setSsrCycles] = useState(15420);
  const [autotuneTemp, setAutotuneTemp] = useState(600);
  const [pidInfo, setPidInfo] = useState<any>(null);
  const [pidLoading, setPidLoading] = useState(false);
  const [faultBusy, setFaultBusy] = useState(false);
  const [showPidResetConfirm, setShowPidResetConfirm] = useState(false);
  const [showAutotuneStartConfirm, setShowAutotuneStartConfirm] = useState(false);
  const [showAutotuneStopConfirm, setShowAutotuneStopConfirm] = useState(false);
  const [confirmBusy, setConfirmBusy] = useState<null | 'pid_reset' | 'autotune_start' | 'autotune_stop'>(null);
  const [otaFile, setOtaFile] = useState<File | null>(null);
  const [otaSha256, setOtaSha256] = useState('');
  const [otaBusy, setOtaBusy] = useState(false);
  const [remoteLoading, setRemoteLoading] = useState(false);
  const [remoteSaving, setRemoteSaving] = useState(false);
  const [remoteConfig, setRemoteConfig] = useState({
    enabled: false,
    uri: '',
    username: '',
    password: '',
    clearPassword: false,
    authKey: '',
    clearAuthKey: false,
    requireSignedCommands: true,
    caPem: '',
    clearCaCert: false,
    deviceId: '',
    hasPassword: false,
    hasAuthKey: false,
    hasCaCert: false,
    connected: false
  });
  const { state: deviceState, connected } = useDeviceState();
  const autotuneInfo = deviceState.autotune || null;

  useEffect(() => {
    // Load from local storage or API
    const saved = localStorage.getItem('kiln_settings');
    if (saved) {
      try {
        const parsed = JSON.parse(saved);
        setSettings((prev) => ({ ...prev, ...parsed }));
      } catch {}
    }
  }, []);
  const loadControllerSettings = async () => {
    try {
      const res = await getJson<any>('/settings');
      if (!res.ok || !res.data) return;
      const d = res.data;
      if (typeof d.temp_offset_c === 'number') setOffset(d.temp_offset_c);
      if (typeof d.ssrCycles === 'number') setSsrCycles(d.ssrCycles);
      if (typeof d.wattage === 'number') setSettings((prev:any) => ({...prev, wattage: d.wattage}));
      if (typeof d.maxC === 'number') setSettings((prev:any) => ({...prev, maxC: d.maxC}));
      if (typeof d.costPerKwh === 'number') setSettings((prev:any) => ({...prev, costPerKwh: d.costPerKwh}));
      if (typeof d.currency === 'string') setSettings((prev:any) => ({...prev, currency: d.currency}));
      if (typeof d.zones === 'number') setSettings((prev:any) => ({...prev, zones: d.zones}));
    } catch (e) {}
  };
  const loadPid = async () => {
    setPidLoading(true);
    try {
      const res = await getJson<any>('/pid');
      if (res.ok) setPidInfo(res.data);
    } catch (e) {}
    setPidLoading(false);
  };

  const loadRemoteConfig = async () => {
    setRemoteLoading(true);
    try {
      const res = await getJson<any>('/api/remote');
      if (res.ok && res.data) {
        const d = res.data;
        setRemoteConfig((prev) => ({
          ...prev,
          enabled: !!d.enabled,
          uri: typeof d.uri === 'string' ? d.uri : '',
          username: typeof d.username === 'string' ? d.username : '',
          password: '',
          clearPassword: false,
          authKey: '',
          clearAuthKey: false,
          requireSignedCommands: d.require_signed_commands !== false,
          caPem: '',
          clearCaCert: false,
          deviceId: typeof d.device_id === 'string' ? d.device_id : '',
          hasPassword: !!d.has_password,
          hasAuthKey: !!d.has_auth_key,
          hasCaCert: !!d.has_ca_cert,
          connected: !!d.connected
        }));
      }
    } catch (e) {
      // ignore, handled by UI state
    } finally {
      setRemoteLoading(false);
    }
  };

  const saveRemoteConfig = async () => {
    try {
      setRemoteSaving(true);
      const payload: any = {
        enabled: remoteConfig.enabled,
        uri: remoteConfig.uri.trim(),
        username: remoteConfig.username.trim(),
        device_id: remoteConfig.deviceId.trim()
      };
      if (remoteConfig.password.trim().length > 0) payload.password = remoteConfig.password;
      if (remoteConfig.clearPassword) payload.clear_password = true;
      if (remoteConfig.authKey.trim().length > 0) payload.auth_key = remoteConfig.authKey;
      if (remoteConfig.clearAuthKey) payload.clear_auth_key = true;
      payload.require_signed_commands = remoteConfig.requireSignedCommands;
      if (remoteConfig.caPem.trim().length > 0) payload.ca_pem = remoteConfig.caPem;
      if (remoteConfig.clearCaCert) payload.clear_ca_cert = true;

      const res = await postJson<any>('/api/remote', payload);
      if (!res.ok) {
        toastApiError(res, 'Failed to save remote access config');
        return;
      }
      toast.success('Remote access settings saved');
      await loadRemoteConfig();
    } catch (e) {
      toast.error('Connection error to controller.');
    } finally {
      setRemoteSaving(false);
    }
  };

  const resetPid = async () => {
    setShowPidResetConfirm(true);
  };

  const confirmResetPid = async () => {
    try {
      setConfirmBusy('pid_reset');
      const res = await postJson<any>('/pid/reset');
      if (res.ok) {
        await loadPid();
        toast.success("PID reset");
        setShowPidResetConfirm(false);
      } else {
        toastApiError(res, "Reset failed");
      }
    } catch (e) {
      toast.error("Connection error to controller.");
    } finally {
      setConfirmBusy(null);
    }
  };

  useEffect(() => {
    loadControllerSettings();
    loadRemoteConfig();

    const onWs = (ev: any) => {
      const msg = ev?.detail;
      if (!msg || !msg.event) return;
      if (msg.event === 'settings_changed') { loadControllerSettings(); loadPid(); loadRemoteConfig(); }
    };
    window.addEventListener('kiln_ws', onWs as any);

    return () => {
      window.removeEventListener('kiln_ws', onWs as any);
    };
  }, []);

  const handleSave = () => {
    localStorage.setItem('kiln_settings', JSON.stringify(settings));
    postJson<any>('/settings', { ...settings, offset, ssrCycles }).then((res) => {
        if (res.ok) toast.success(t.settings.saved);
        else toastApiError(res, "Save failed");
    });
  };

  const handleAutoTune = async () => {
      setShowAutotuneStartConfirm(true);
  };

  const confirmAutoTuneStart = async () => {
      try {
          setConfirmBusy('autotune_start');
          const res = await postJson<any>('/autotune', { temp: autotuneTemp });
          if(res.ok) {
            toast.success(`Autotune started at ${autotuneTemp}°C! Monitor the dashboard.`);
            setShowAutotuneStartConfirm(false);
          } else {
            toastApiError(res, "Unknown error", "Error");
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

  const onOtaFilePicked = async (file: File | null) => {
    setOtaFile(file);
    setOtaSha256('');
    if (!file) return;
    try {
      const buf = await file.arrayBuffer();
      const digest = await crypto.subtle.digest('SHA-256', buf);
      const hash = Array.from(new Uint8Array(digest))
        .map((b) => b.toString(16).padStart(2, '0'))
        .join('');
      setOtaSha256(hash);
    } catch {
      toast.error('Failed to compute SHA-256');
    }
  };

  const uploadFirmware = async () => {
    if (!otaFile || !otaSha256) {
      toast.error('Pick firmware file first');
      return;
    }
    try {
      setOtaBusy(true);
      const bin = await otaFile.arrayBuffer();
      const res = await requestJson<any>('/ota/update', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/octet-stream',
          'X-Firmware-Sha256': otaSha256
        },
        body: bin
      });
      if (!res.ok) {
        toastApiError(res, "OTA upload error", "OTA failed");
        return;
      }
      toast.success('Firmware uploaded. Controller rebooting...');
    } catch {
      toast.error('OTA upload error');
    } finally {
      setOtaBusy(false);
    }
  };

  const confirmAutoTuneStop = async () => {
      try {
          setConfirmBusy('autotune_stop');
          const res = await postJson<any>('/autotune/stop');
          if(res.ok) {
            toast.success("Autotune stopped");
            setShowAutotuneStopConfirm(false);
          } else {
            toastApiError(res, "Stop failed");
          }
      } catch (e) {
          toast.error("Connection error to controller.");
      } finally {
          setConfirmBusy(null);
      }
  };

  const clearFault = async () => {
    if (faultBusy) return;
    try {
      setFaultBusy(true);
      const res = await postJson<any>('/fault/clear');
      if (res.ok) {
        toast.success(t.settings.faultCleared || "Fault cleared");
      } else {
        toastApiError(res, t.settings.faultClearFailed || "Failed to clear fault");
      }
    } catch (e) {
      toast.error("Connection error to controller.");
    } finally {
      setFaultBusy(false);
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
                    <label className="text-xs text-slate-500 font-bold uppercase mb-2 block">Max Allowed Temp (°C)</label>
                    <input
                        className="w-full bg-black border border-zinc-800 rounded-lg p-3 text-white focus:border-blue-500 focus:outline-none transition-colors"
                        type="number"
                        min="100"
                        max="1300"
                        step="1"
                        value={settings.maxC}
                        onChange={e => setSettings({...settings, maxC: Math.max(100, Math.min(1300, Number(e.target.value) || 1300))})}
                    />
                </div>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
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
         <h2 className="text-lg font-bold text-white mb-4 flex items-center gap-3 border-b border-zinc-800 pb-4">
            <Globe size={24} className="text-cyan-400" />
            {t.settings.remoteAccess || "Remote Access (Internet)"}
        </h2>
        <div className="space-y-4">
            <div className="grid grid-cols-1 md:grid-cols-3 gap-3">
                <div className="md:col-span-2">
                    <label className="text-xs text-slate-500 font-bold uppercase mb-2 block">
                      {t.settings.mqttBroker || "MQTT Broker URI"}
                    </label>
                    <input
                      type="text"
                      placeholder="mqtts://broker.example.com:8883"
                      className="w-full bg-black border border-zinc-800 rounded-lg p-3 text-white focus:border-cyan-500 focus:outline-none transition-colors"
                      value={remoteConfig.uri}
                      onChange={(e) => setRemoteConfig((prev) => ({ ...prev, uri: e.target.value }))}
                    />
                </div>
                <div>
                    <label className="text-xs text-slate-500 font-bold uppercase mb-2 block">
                      {t.settings.remoteStatus || "Remote Status"}
                    </label>
                    <div className={`h-[46px] rounded-lg border px-3 flex items-center font-mono text-sm ${
                      remoteConfig.connected ? 'border-emerald-700 bg-emerald-900/20 text-emerald-300' : 'border-zinc-800 bg-black text-zinc-400'
                    }`}>
                      {remoteLoading ? 'loading...' : (remoteConfig.connected ? 'connected' : 'offline')}
                    </div>
                </div>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-3 gap-3">
                <div>
                    <label className="text-xs text-slate-500 font-bold uppercase mb-2 block">
                      {t.settings.remoteUser || "MQTT Username"}
                    </label>
                    <input
                      type="text"
                      className="w-full bg-black border border-zinc-800 rounded-lg p-3 text-white focus:border-cyan-500 focus:outline-none transition-colors"
                      value={remoteConfig.username}
                      onChange={(e) => setRemoteConfig((prev) => ({ ...prev, username: e.target.value }))}
                    />
                </div>
                <div>
                    <label className="text-xs text-slate-500 font-bold uppercase mb-2 block">
                      {t.settings.remotePassword || "MQTT Password"}
                    </label>
                    <input
                      type="password"
                      autoComplete="new-password"
                      placeholder={remoteConfig.hasPassword ? '••••••••' : ''}
                      className="w-full bg-black border border-zinc-800 rounded-lg p-3 text-white focus:border-cyan-500 focus:outline-none transition-colors"
                      value={remoteConfig.password}
                      onChange={(e) => setRemoteConfig((prev) => ({ ...prev, password: e.target.value, clearPassword: false }))}
                    />
                </div>
                <div>
                    <label className="text-xs text-slate-500 font-bold uppercase mb-2 block">
                      {t.settings.authKey || "Command Auth Key"}
                    </label>
                    <input
                      type="password"
                      autoComplete="new-password"
                      placeholder={remoteConfig.hasAuthKey ? '••••••••' : ''}
                      className="w-full bg-black border border-zinc-800 rounded-lg p-3 text-white focus:border-cyan-500 focus:outline-none transition-colors"
                      value={remoteConfig.authKey}
                      onChange={(e) => setRemoteConfig((prev) => ({ ...prev, authKey: e.target.value, clearAuthKey: false }))}
                    />
                </div>
                <div>
                    <label className="text-xs text-slate-500 font-bold uppercase mb-2 block">
                      {t.settings.deviceId || "Device ID"}
                    </label>
                    <input
                      type="text"
                      className="w-full bg-black border border-zinc-800 rounded-lg p-3 text-white focus:border-cyan-500 focus:outline-none transition-colors font-mono"
                      value={remoteConfig.deviceId}
                      onChange={(e) => setRemoteConfig((prev) => ({ ...prev, deviceId: e.target.value }))}
                    />
                </div>
            </div>

            <div className="flex flex-col md:flex-row md:items-center md:justify-between gap-3 rounded-lg border border-zinc-800 bg-black/20 p-3">
              <label className="inline-flex items-center gap-3 text-sm text-zinc-200">
                <input
                  type="checkbox"
                  className="h-4 w-4 accent-cyan-500"
                  checked={remoteConfig.enabled}
                  onChange={(e) => setRemoteConfig((prev) => ({ ...prev, enabled: e.target.checked }))}
                />
                {t.settings.enableRemote || "Enable remote access through Internet"}
              </label>
              <label className="inline-flex items-center gap-2 text-xs text-zinc-400">
                <input
                  type="checkbox"
                  className="h-4 w-4 accent-red-500"
                  checked={remoteConfig.clearPassword}
                  onChange={(e) => setRemoteConfig((prev) => ({ ...prev, clearPassword: e.target.checked, password: '' }))}
                />
                {t.settings.clearRemotePassword || "Clear saved password on controller"}
              </label>
            </div>
            <div className="flex flex-col md:flex-row md:items-center md:justify-between gap-3 rounded-lg border border-zinc-800 bg-black/20 p-3">
              <label className="inline-flex items-center gap-3 text-sm text-zinc-200">
                <input
                  type="checkbox"
                  className="h-4 w-4 accent-amber-500"
                  checked={remoteConfig.requireSignedCommands}
                  onChange={(e) => setRemoteConfig((prev) => ({ ...prev, requireSignedCommands: e.target.checked }))}
                />
                {t.settings.requireSignedCommands || "Require signed remote commands (HMAC)"}
              </label>
              <label className="inline-flex items-center gap-2 text-xs text-zinc-400">
                <input
                  type="checkbox"
                  className="h-4 w-4 accent-red-500"
                  checked={remoteConfig.clearAuthKey}
                  onChange={(e) => setRemoteConfig((prev) => ({ ...prev, clearAuthKey: e.target.checked, authKey: '' }))}
                />
                {t.settings.clearAuthKey || "Clear command auth key on controller"}
              </label>
            </div>

            <div className="text-xs text-zinc-500 font-mono bg-black/20 border border-zinc-800 rounded-lg p-3">
              Topic: trae/{remoteConfig.deviceId || 'DEVICE_ID'}/cmd
            </div>
            <div>
              <label className="text-xs text-slate-500 font-bold uppercase mb-2 block">
                {t.settings.caCert || "Broker CA Certificate (PEM)"}
              </label>
              <textarea
                rows={5}
                placeholder={remoteConfig.hasCaCert ? 'CA cert is saved on controller (paste to replace)' : '-----BEGIN CERTIFICATE----- ...'}
                className="w-full bg-black border border-zinc-800 rounded-lg p-3 text-white focus:border-cyan-500 focus:outline-none transition-colors font-mono text-xs"
                value={remoteConfig.caPem}
                onChange={(e) => setRemoteConfig((prev) => ({ ...prev, caPem: e.target.value, clearCaCert: false }))}
              />
              <label className="inline-flex items-center gap-2 text-xs text-zinc-400 mt-2">
                <input
                  type="checkbox"
                  className="h-4 w-4 accent-red-500"
                  checked={remoteConfig.clearCaCert}
                  onChange={(e) => setRemoteConfig((prev) => ({ ...prev, clearCaCert: e.target.checked, caPem: '' }))}
                />
                {t.settings.clearCaCert || "Clear saved CA certificate"}
              </label>
            </div>

            <button
              className="w-full py-3 bg-cyan-700 hover:bg-cyan-600 disabled:bg-zinc-700 disabled:text-zinc-400 text-white rounded-lg font-medium transition-colors flex items-center justify-center gap-2"
              onClick={saveRemoteConfig}
              disabled={remoteSaving || remoteLoading}
            >
              <Save size={18} /> {remoteSaving ? (t.settings.saving || 'Saving...') : (t.settings.saveRemote || "Save Remote Access")}
            </button>
        </div>
      </div>

      <div className="bg-kiln-card border border-kiln-border rounded-xl p-6 shadow-lg">
         <h2 className="text-lg font-bold text-white mb-6 flex items-center gap-3 border-b border-zinc-800 pb-4">
            <Shield size={24} className="text-red-400" />
            {t.settings.safety}
        </h2>

        <div className="mb-6 grid grid-cols-1 md:grid-cols-3 gap-3">
            <div className="bg-black/20 border border-zinc-800 rounded-lg p-3">
                <div className="text-[11px] uppercase text-zinc-500 font-bold">Link</div>
                <div className="text-sm font-mono text-zinc-200 break-all">{deviceState.server_url || '-'}</div>
            </div>
            <div className="bg-black/20 border border-zinc-800 rounded-lg p-3">
                <div className="text-[11px] uppercase text-zinc-500 font-bold">{t.settings.fault || "Fault"}</div>
                <div className={`text-sm font-mono ${deviceState.fault_active ? 'text-red-400' : 'text-emerald-400'}`}>
                    {deviceState.fault_active ? `ACTIVE (${deviceState.fault_code ?? '-'})` : `NONE (${deviceState.fault_code ?? 0})`}
                </div>
                <div className="text-xs text-zinc-500 mt-1 break-words">
                    {t.settings.faultReason || "Reason"}: {deviceState.fault_reason || '-'}
                </div>
                <button
                  onClick={clearFault}
                  disabled={!deviceState.fault_active || faultBusy}
                  className="mt-2 w-full py-2 bg-red-600 hover:bg-red-500 disabled:bg-zinc-800 disabled:text-zinc-500 text-white rounded-lg font-bold transition-colors text-xs"
                >
                  {faultBusy ? (t.settings.clearing || "Clearing...") : (t.settings.clearFault || "Clear Fault")}
                </button>
            </div>
            <div className="bg-black/20 border border-zinc-800 rounded-lg p-3">
                <div className="text-[11px] uppercase text-zinc-500 font-bold">Uptime</div>
                <div className="text-sm font-mono text-zinc-200">
                    {typeof deviceState.uptime_ms === 'number' ? `${Math.floor(deviceState.uptime_ms / 1000)}s` : '-'} {connected ? '• online' : '• offline'}
                </div>
            </div>
        </div>

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
                <p className="text-zinc-500 text-sm mb-3">{t.settings.firmwareDesc}</p>
                <input
                  type="file"
                  accept=".bin,application/octet-stream"
                  onChange={(e) => onOtaFilePicked(e.target.files?.[0] || null)}
                  className="w-full text-sm text-zinc-300 file:mr-3 file:rounded-lg file:border-0 file:bg-zinc-800 file:px-3 file:py-2 file:text-zinc-100 hover:file:bg-zinc-700"
                />
                {otaSha256 && (
                  <div className="mt-2 text-[11px] font-mono text-zinc-500 break-all">
                    SHA-256: {otaSha256}
                  </div>
                )}
                <button
                  onClick={uploadFirmware}
                  disabled={!otaFile || !otaSha256 || otaBusy}
                  className="mt-3 w-full py-3 bg-blue-600 hover:bg-blue-500 disabled:bg-zinc-700 disabled:text-zinc-400 text-white rounded-lg font-medium transition-colors flex items-center justify-center gap-2 shadow-lg shadow-blue-900/20"
                >
                    <Upload size={18} /> {otaBusy ? 'Uploading...' : t.settings.uploadBin}
                </button>
            </div>
        </div>
      </div>
    </div>
  );
};

export default Settings;
