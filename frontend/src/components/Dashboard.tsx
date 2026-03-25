import React, { useCallback, useEffect, useState } from 'react';
import { Line } from 'react-chartjs-2';
import { Chart as ChartJS, CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler } from 'chart.js';
import { Thermometer, Activity, Clock, Wind, DollarSign, Sliders, Play, Square, Timer, ChevronDown, Check } from 'lucide-react';
import { useLanguage } from '../contexts/LanguageContext';
import { useSchedules, Schedule } from '../contexts/SchedulesContext';
import { useDeviceState } from '../contexts/DeviceStateContext';
import toast from 'react-hot-toast';
import { ConfirmModal } from './ConfirmModal';
import { postCommand } from '../api/commands';
import { getJson, postJson } from '../api/http';
import { toastApiError } from '../api/notify';

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler);

const chartOptions = {
  responsive: true,
  maintainAspectRatio: false,
  interaction: { mode: 'index' as const, intersect: false },
  plugins: {
    legend: { display: false },
    tooltip: { 
        backgroundColor: '#18181b', 
        titleColor: '#fff', 
        bodyColor: '#a1a1aa', 
        borderColor: '#27272a', 
        borderWidth: 1,
        padding: 10,
        displayColors: false,
        callbacks: {
            label: (context: any) => `${context.dataset.label}: ${context.raw}°C`
        }
    }
  },
  scales: {
    x: { 
        grid: { color: '#27272a', drawBorder: false }, 
        ticks: { color: '#71717a', font: {family: 'Inter', size: 10}, maxRotation: 0 } 
    },
    y: { 
        min: 0, 
        max: 1300, 
        grid: { color: '#27272a', drawBorder: false }, 
        ticks: { color: '#71717a', font: {family: 'Inter', size: 10}, stepSize: 350 } 
    }
  }
};

const StatCard = ({ title, value, unit, colorClass, valueClass, subValue, subValueClass, className }: any) => (
  <div className={`bg-kiln-card border kiln-border rounded-xl p-4 flex flex-col justify-center shadow-lg shadow-black/20 min-w-0 h-24 relative overflow-hidden ${className || ''}`}>
    <div className="text-[10px] font-bold text-zinc-500 uppercase tracking-widest mb-2 truncate">{title}</div>
    <div className={`text-xl lg:text-2xl font-bold tracking-tight truncate ${valueClass || 'text-white'}`}>
      {value} <span className="text-sm text-zinc-600 font-normal ml-0.5 align-baseline">{unit}</span>
    </div>
    {subValue && (
        <div className={`text-xs font-medium mt-1 ${subValueClass || 'text-zinc-500'}`}>
            {subValue}
        </div>
    )}
  </div>
);

interface StatusData {
    temp: number;
    target: number;
    output: number;
    status: string;
    step: number;
    totalSteps: number;
    time?: string;
    pcbTemp?: number;
    error?: string;
    history?: {x: number, y: number}[];
    schedules_rev?: number;
    settings_rev?: number;
    autotune?: {
        active: boolean;
        heater_on?: boolean;
        setpoint_c?: number;
        cycles?: number;
        ku?: number;
        pu_s?: number;
        kp?: number;
        ki?: number;
        kd?: number;
    };
    fan_manual?: boolean;
    fan_auto?: boolean;
    fan_power?: number;
    fan_effective_power?: number;
}

interface FanControl {
    manual: boolean;
    auto: boolean;
    power: number;
    temp_min_c: number;
    temp_max_c: number;
    power_min: number;
    power_max: number;
    effective_power?: number;
}

const Dashboard = () => {
  const { t, language } = useLanguage();
  const { schedules, getScheduleDetails, refreshSchedules } = useSchedules();
  const { state: deviceState } = useDeviceState();
  
  const [status, setStatus] = useState<StatusData>({ 
      temp: 25, 
      target: 0, 
      output: 0, 
      status: 'IDLE', 
      step: 0, 
      totalSteps: 0,
      pcbTemp: 35.5,
      history: []
  });
  
  const [history, setHistory] = useState<{x: number, y: number}[]>([]);
  const [fan, setFan] = useState<FanControl>({
      manual: false,
      auto: true,
      power: 0,
      temp_min_c: 45,
      temp_max_c: 280,
      power_min: 20,
      power_max: 100,
      effective_power: 0
  });
  const [fanBusy, setFanBusy] = useState(false);
  const [faultBusy, setFaultBusy] = useState(false);
  const [selectedScheduleId, setSelectedScheduleId] = useState<string>("");
  const [isScheduleMenuOpen, setIsScheduleMenuOpen] = useState(false);
  const getScheduleStepCount = (schedule: Schedule | undefined) => {
      if (!schedule) return 0;
      const local = Array.isArray(schedule.steps) ? schedule.steps.length : 0;
      const remote = typeof schedule.stepsCount === 'number' ? schedule.stepsCount : 0;
      return Math.max(local, remote);
  };

  const applyStatusFrame = useCallback((d: any) => {
      if (!d || typeof d !== 'object') return;

      const hasStatusSnapshot =
          'temp' in d || 'target' in d || 'output' in d || 'status' in d || 'timeRemaining' in d;

      if (hasStatusSnapshot) {
          setStatus(prev => ({
              ...prev,
              ...d
          }));
      }

      setFan(prev => ({
          ...prev,
          manual: typeof d.fan_manual === 'boolean' ? d.fan_manual : prev.manual,
          auto: typeof d.fan_auto === 'boolean' ? d.fan_auto : prev.auto,
          power: typeof d.fan_power === 'number' ? d.fan_power : prev.power,
          effective_power: typeof d.fan_effective_power === 'number' ? d.fan_effective_power : prev.effective_power
      }));

      if (Array.isArray(d.history)) {
          setHistory(d.history);
      }
  }, []);

  // Unified Idle Logic
  const isIdle = status.status === 'IDLE' || status.status === 'COMPLETE' || status.status === 'ERROR' || status.status === 'FAULT';

  // Derived active schedule
  const activeSchedule = schedules.find(s => s.id === selectedScheduleId);

  // Helper to calculate target profile points
  const getTargetProfile = (schedule: Schedule | undefined) => {
      if (!schedule || !schedule.steps) return [];
      const points = [{x: 0, y: 25}];
      let currentTime = 0;
      let currentTemp = 25;

      schedule.steps.forEach(step => {
          if (step.type === 'ramp') {
              const diff = Math.abs(step.target - currentTemp);
              const rate = step.rate || 100;
              const duration = diff / rate;
              currentTime += duration;
              currentTemp = Number(step.target);
              points.push({x: currentTime, y: currentTemp});
          } else {
              const duration = (step.holdTime || 0) / 60;
              currentTime += duration;
              points.push({x: currentTime, y: currentTemp});
          }
      });
      return points;
  };
  
  // Load details when selected (if missing)
  useEffect(() => {
      if (activeSchedule) {
          if (!activeSchedule.steps || activeSchedule.steps.length === 0) {
              getScheduleDetails(activeSchedule);
          }
      }
  }, [selectedScheduleId, schedules]);

  const loadFan = async () => {
    const res = await getJson<any>('/fan');
    if (res.ok && res.data) {
      const d = res.data;
      setFan(prev => ({
        ...prev,
        ...d
      }));
    }
  };

  useEffect(() => {
    loadFan();
  }, []);

  useEffect(() => {
    applyStatusFrame(deviceState);
  }, [deviceState, applyStatusFrame]);

  useEffect(() => {
    const onWs = (ev: any) => {
      const msg = ev?.detail;
      if (!msg || !msg.event) return;
      if (msg.event === 'settings_changed') {
        loadFan();
      }
    };
    window.addEventListener('kiln_ws', onWs as any);

    return () => {
      window.removeEventListener('kiln_ws', onWs as any);
    };
  }, []);

  const handleStartFiring = async () => {
      if (!activeSchedule) {
          alert("Please select a schedule first");
          return;
      }

      const res = await postCommand('/start', { schedule: activeSchedule });
      if (!res.ok) {
          alert("Error: " + (res.message || "Start Failed"));
      }
  };

  const [showStopConfirm, setShowStopConfirm] = useState(false);
  const [showAutotuneStopConfirm, setShowAutotuneStopConfirm] = useState(false);
  const [confirmBusy, setConfirmBusy] = useState<null | 'stop_firing' | 'stop_autotune'>(null);
  const [showAddTimeModal, setShowAddTimeModal] = useState(false);
  const [showAddTempModal, setShowAddTempModal] = useState(false);

  const handleStopFiring = async () => {
      setShowStopConfirm(true);
  };

  const saveFan = async (patch: Partial<FanControl>) => {
      const next: FanControl = { ...fan, ...patch };
      setFan(next);
      try {
          setFanBusy(true);
          const res = await postJson<any>('/fan', next);
          if (!res.ok) {
              toastApiError(res, "Fan save failed");
          }
      } catch {
          toast.error("Fan control connection error");
      } finally {
          setFanBusy(false);
      }
  };

  const fanSpeed = Number(fan.effective_power ?? 0);
  const isFanEnabled = fanSpeed > 0;
  const toggleFanPower = () => {
      if (isFanEnabled) {
          saveFan({ auto: false, manual: true, power: 0 });
      } else {
          saveFan({ auto: false, manual: true, power: 60 });
      }
  };
  const nudgeFanManual = (delta: number) => {
      const nextRaw = Math.max(0, Math.min(100, Number(fan.power || 0) + delta));
      const next = nextRaw > 0 ? Math.max(60, nextRaw) : 0;
      saveFan({ auto: false, manual: true, power: next });
  };

  const handleAutotuneStop = async () => {
      setShowAutotuneStopConfirm(true);
      return;
      try {
          const res = await fetch(`${API_BASE_URL}/autotune/stop`, { method: 'POST' });
          if (!res.ok) alert("Stop failed");
      } catch (e) {
          alert("Connection error");
      }
  };

  const handleSkip = async () => {
      await postCommand('/skip');
  };

  const submitAddTime = async (minutes: number, stepIndex: number) => {
      await postCommand('/addTime', { minutes, stepIndex });
  };

  const handleAddTimeClick = () => {
      setShowAddTimeModal(true);
  };

  const submitAddTemp = async (degrees: number, stepIndex: number) => {
      await postCommand('/addTemp', { degrees, stepIndex });
  };

  const handleAddTempClick = () => {
      setShowAddTempModal(true);
  };

  const confirmStop = async () => {
      try {
          setConfirmBusy('stop_firing');
          const res = await postCommand('/stop');
          if (!res.ok) throw new Error(res.message || 'stop failed');
          setShowStopConfirm(false);
      } catch (e) {
          toast.error("Error stopping firing");
      } finally {
          setConfirmBusy(null);
      }
  };

  const confirmAutotuneStop = async () => {
      try {
          setConfirmBusy('stop_autotune');
          const res = await postJson<any>('/autotune/stop');
          if (!res.ok) throw new Error(res.message || 'stop failed');
          toast.success("Autotune stopped");
          setShowAutotuneStopConfirm(false);
      } catch (e) {
          toast.error("Stop Autotune failed");
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

  const getSegmentInfo = () => {
      if (!isIdle) {
          return `${status.step || 1} / ${status.totalSteps || activeSchedule?.steps?.length || '?'}`;
      }
      if (activeSchedule && activeSchedule.steps) {
          return `${t.dashboard.total}: ${activeSchedule.steps.length}`;
      }
      return "-- / --";
  };

  const formatTimeRemaining = (status: any) => {
      if (status.status === 'IDLE') return "--:--";
      if (status.status === 'COMPLETE') return t.dashboard.done;
      if (status.timeRemaining !== undefined) {
          const hours = Math.floor(status.timeRemaining / 60);
          const mins = Math.floor(status.timeRemaining % 60);
          return `${hours}${t.dashboard.hourSuffix} ${mins}${t.dashboard.minSuffix}`;
      }
      return "--:--";
  };

  return (
    <div className="flex h-full gap-6 p-4 md:p-6 max-w-[1600px] mx-auto overflow-hidden flex-col md:flex-row">
      
      {/* LEFT SIDEBAR: SCHEDULE LIBRARY (Desktop Only) */}
      <div className="hidden md:flex w-72 flex-col gap-3 shrink-0 bg-kiln-card border border-kiln-border rounded-xl p-3 shadow-lg overflow-hidden">
          <button
              onClick={() => { setIsScheduleMenuOpen(true); refreshSchedules(); }}
              className="w-full bg-zinc-900/60 border border-zinc-800 rounded-xl p-3 text-left hover:border-zinc-700 transition-colors"
          >
              <div className="text-[10px] uppercase tracking-widest font-bold text-zinc-500 mb-2 flex items-center gap-2">
                  <Sliders size={14} className="text-kiln-accent" />
                  {t.dashboard.selectSchedule}
              </div>
              <div className="text-sm font-bold text-white truncate">
                  {activeSchedule?.name || t.dashboard.selectSchedule}
              </div>
              <div className="text-[11px] text-zinc-500 mt-1">
                  {activeSchedule ? `${getScheduleStepCount(activeSchedule)} ${t.schedules.steps}` : t.schedules.selectToEdit}
              </div>
          </button>

          <div className="bg-zinc-900/60 border border-zinc-800 rounded-xl p-3">
              <div className="text-[10px] uppercase tracking-widest font-bold text-zinc-500 mb-2">
                  {t.dashboard.currentTemp}
              </div>
              <div className="text-3xl font-bold text-white tracking-tight leading-none">
                  {(status.temp || 0).toFixed(1)}
                  <span className="text-lg text-zinc-500 ml-1">°C</span>
              </div>
              <div className="mt-2 text-xs font-semibold">
                  <span className="text-zinc-500 mr-2">{t.dashboard.status || 'Status'}:</span>
                  <span className={isIdle ? 'text-zinc-400' : 'text-kiln-accent'}>
                      {(t.status as Record<string, string>)[status.status] || status.status}
                  </span>
              </div>
          </div>

          <div className="bg-zinc-900/60 border border-zinc-800 rounded-xl p-3">
              <div className="text-[10px] uppercase tracking-widest font-bold text-zinc-500 mb-2">
                  {t.dashboard.segment}
              </div>
              <div className="text-xl font-bold text-purple-400 leading-none">
                  {getSegmentInfo()}
              </div>
          </div>

          <div className="bg-zinc-900/60 border border-zinc-800 rounded-xl p-3">
              <div className="text-[10px] uppercase tracking-widest font-bold text-zinc-500 mb-2">
                  {t.dashboard.timeRemaining}
              </div>
              <div className="text-xl font-bold text-white leading-none">
                  {formatTimeRemaining(status)}
              </div>
          </div>

          <div className="bg-zinc-900/60 border border-zinc-800 rounded-xl p-3 flex items-center gap-3">
              <button
                  onClick={toggleFanPower}
                  disabled={fanBusy}
                  className="p-2 rounded-md"
                  title={isFanEnabled ? 'Fan ON' : 'Fan OFF'}
              >
                  <Wind size={30} className={isFanEnabled ? 'text-emerald-400' : 'text-zinc-500'} />
              </button>
              <div className="flex-1 text-right">
                  <div className="text-lg font-bold text-white tabular-nums leading-none">{fanSpeed}%</div>
                  <div className="text-[10px] text-zinc-500 mt-1">PCB {Number(status.pcbTemp ?? 0).toFixed(1)}°C</div>
              </div>
              <div className="flex flex-row items-center gap-2">
                  <button
                      onClick={() => nudgeFanManual(-5)}
                      disabled={fanBusy}
                      className="w-8 h-8 rounded-md bg-zinc-800 text-zinc-200 text-xl leading-none active:scale-95"
                      title="Decrease fan power"
                  >
                      -
                  </button>
                  <button
                      onClick={() => nudgeFanManual(5)}
                      disabled={fanBusy}
                      className="w-8 h-8 rounded-md bg-zinc-800 text-zinc-200 text-xl leading-none active:scale-95"
                      title="Increase fan power"
                  >
                      +
                  </button>
              </div>
          </div>

      </div>

      {/* RIGHT CONTENT: DASHBOARD & CONTROLS */}
      <div className="flex-1 flex flex-col gap-6 overflow-y-auto pb-20 md:pb-0 relative no-scrollbar md:scrollbar-default">
      
      {/* Mobile Schedule Selector (Sticky Top) */}
      <div className="md:hidden sticky top-0 z-30 bg-kiln-bg/95 backdrop-blur-sm pb-2 pt-1">
          <button 
              onClick={() => { setIsScheduleMenuOpen(true); refreshSchedules(); }}
              className="w-full bg-zinc-800 border border-zinc-700 p-4 rounded-xl flex justify-between items-center text-white font-bold shadow-lg active:scale-98 transition-transform"
          >
              <span className="flex items-center gap-2">
                  <Sliders size={18} className="text-kiln-accent" />
                  {activeSchedule?.name || t.dashboard.selectSchedule}
              </span>
              <ChevronDown size={20} className="text-zinc-500" />
          </button>
      </div>

      {deviceState.fault_active && (
        <div className="bg-red-950/40 border border-red-600/40 rounded-xl p-4 flex flex-col md:flex-row gap-3 md:items-center md:justify-between shadow-lg">
            <div>
                <div className="text-xs uppercase tracking-widest text-red-300 font-bold">
                    {t.settings.fault || "Fault"}
                </div>
                <div className="text-sm text-red-100 font-mono">
                    {t.settings.faultReason || "Reason"}: {deviceState.fault_reason || `Code ${deviceState.fault_code ?? '-'}`}
                </div>
            </div>
            <button
                onClick={clearFault}
                disabled={faultBusy}
                className="px-4 py-2 bg-red-600 hover:bg-red-500 disabled:bg-zinc-800 disabled:text-zinc-500 text-white rounded-lg font-bold transition-colors text-sm whitespace-nowrap"
            >
                {faultBusy ? (t.settings.clearing || "Clearing...") : (t.settings.clearFault || "Clear Fault")}
            </button>
        </div>
      )}

      <ConfirmModal
          open={showStopConfirm}
          title={`${t.dashboard.stopFiring}?`}
          description="Are you sure you want to abort the current firing process? This action cannot be undone."
          confirmText={t.dashboard.stopFiring}
          cancelText="Cancel"
          busy={confirmBusy === 'stop_firing'}
          icon={<Square size={40} fill="currentColor" />}
          onConfirm={confirmStop}
          onCancel={() => setShowStopConfirm(false)}
      />

      <ConfirmModal
          open={showAutotuneStopConfirm}
          title="Stop Autotune?"
          description="Are you sure you want to stop Autotune now? Heater will turn OFF and results may be incomplete."
          confirmText="STOP Autotune"
          cancelText="Cancel"
          busy={confirmBusy === 'stop_autotune'}
          icon={<Square size={40} fill="currentColor" />}
          onConfirm={confirmAutotuneStop}
          onCancel={() => setShowAutotuneStopConfirm(false)}
      />

      {/* MOBILE CONTROL BAR (Fixed Bottom) */}
      <div className="md:hidden fixed bottom-16 left-0 right-0 bg-zinc-900/95 backdrop-blur-md border-t border-zinc-800 p-3 z-40 shadow-[0_-5px_20px_rgba(0,0,0,0.5)]">
          {isIdle ? (
              <button 
                  onClick={handleStartFiring}
                  disabled={!activeSchedule}
                  className={`w-full py-3 rounded-xl transition-all text-lg font-bold flex items-center justify-center gap-2 ${activeSchedule ? 'bg-kiln-accent text-black hover:bg-emerald-400 shadow-[0_0_15px_rgba(16,185,129,0.2)]' : 'bg-zinc-800 text-zinc-500 cursor-not-allowed'}`}
              >
                  <Play size={20} fill={activeSchedule ? "black" : "currentColor"} /> 
                  {activeSchedule ? t.dashboard.startFiring : t.dashboard.selectSchedule}
              </button>
          ) : (
              <div className="flex flex-col gap-2">
                  <div className="grid grid-cols-3 gap-2">
                      <button onClick={handleAddTimeClick} className="py-2.5 bg-zinc-800 rounded-lg text-[10px] font-bold text-zinc-300 flex flex-col items-center justify-center gap-1 border border-zinc-700 active:bg-zinc-700">
                          <Clock size={16} /> {t.dashboard.addTime}
                      </button>
                      <button onClick={handleAddTempClick} className="py-2.5 bg-zinc-800 rounded-lg text-[10px] font-bold text-zinc-300 flex flex-col items-center justify-center gap-1 border border-zinc-700 active:bg-zinc-700">
                          <Thermometer size={16} /> {t.dashboard.addTemp}
                      </button>
                      <button onClick={handleSkip} className="py-2.5 bg-zinc-800 rounded-lg text-[10px] font-bold text-zinc-300 flex flex-col items-center justify-center gap-1 border border-zinc-700 active:bg-zinc-700">
                          <Play size={16} className="fill-current" /> {t.dashboard.skip}
                      </button>
                  </div>
                  <button 
                      onClick={handleStopFiring}
                      className="w-full py-3 bg-red-600 text-white rounded-xl hover:bg-red-500 transition-all text-lg font-bold shadow-[0_0_15px_rgba(239,68,68,0.2)] flex items-center justify-center gap-2 active:scale-[0.98]"
                  >
                      <Square size={20} fill="white" /> {t.dashboard.stopFiring}
                  </button>
              </div>
          )}
      </div>

      {/* Add Time Modal */}
      {showAddTimeModal && (
          <div className="fixed inset-0 z-[100] bg-black/80 backdrop-blur-sm flex items-center justify-center p-4">
              <div className="bg-kiln-card border border-zinc-700 rounded-2xl p-6 max-w-md w-full shadow-2xl animate-in fade-in zoom-in duration-200">
                  <div className="flex justify-between items-center mb-4">
                      <h3 className="text-xl font-bold text-white">{t.dashboard.extendHoldTitle}</h3>
                      <button onClick={() => setShowAddTimeModal(false)} className="text-zinc-400 hover:text-white">
                          <Check size={20} />
                      </button>
                  </div>
                  
                  <div className="space-y-2 max-h-[60vh] overflow-y-auto pr-2 no-scrollbar md:scrollbar-default">
                      {activeSchedule?.steps.map((step, index) => {
                          if (step.type !== 'hold' || index < (status.step - 1)) return null;
                          return (
                              <div key={index} className="flex justify-between items-center p-3 bg-zinc-900/50 rounded-lg border border-zinc-800">
                                  <div>
                                      <div className="text-sm font-bold text-zinc-300">{t.dashboard.stepHold} {index + 1} {t.dashboard.holdSuffix}</div>
                                      <div className="text-xs text-zinc-500">{t.dashboard.currentDuration}: {step.holdTime}{t.dashboard.minSuffix}</div>
                                  </div>
                                  <div className="flex gap-2">
                                      <button 
                                          onClick={() => submitAddTime(1, index)}
                                          className="px-3 py-1.5 bg-zinc-800 hover:bg-zinc-700 rounded text-xs font-bold text-white transition-colors"
                                      >
                                          +1{t.dashboard.minSuffix}
                                      </button>
                                      <button 
                                          onClick={() => submitAddTime(5, index)}
                                          className="px-3 py-1.5 bg-zinc-800 hover:bg-zinc-700 rounded text-xs font-bold text-white transition-colors"
                                      >
                                          +5{t.dashboard.minSuffix}
                                      </button>
                                      <button 
                                          onClick={() => submitAddTime(10, index)}
                                          className="px-3 py-1.5 bg-zinc-800 hover:bg-zinc-700 rounded text-xs font-bold text-white transition-colors"
                                      >
                                          +10{t.dashboard.minSuffix}
                                      </button>
                                  </div>
                              </div>
                          );
                      })}
                      {activeSchedule?.steps.every((s, i) => s.type !== 'hold' || i < (status.step - 1)) && (
                          <div className="text-center text-zinc-500 py-4">
                              {t.dashboard.noHoldsAvailable}
                          </div>
                      )}
                  </div>
              </div>
          </div>
      )}

      {/* Add Temp Modal */}
      {showAddTempModal && (
          <div className="fixed inset-0 z-[100] bg-black/80 backdrop-blur-sm flex items-center justify-center p-4">
              <div className="bg-kiln-card border border-zinc-700 rounded-2xl p-6 max-w-xl w-full shadow-2xl animate-in fade-in zoom-in duration-200 max-h-[80vh] flex flex-col">
                  <div className="flex justify-between items-center mb-4 shrink-0">
                      <h3 className="text-xl font-bold text-white">{t.dashboard.extendTempTitle}</h3>
                      <button onClick={() => setShowAddTempModal(false)} className="text-zinc-400 hover:text-white">
                          <Check size={20} />
                      </button>
                  </div>
                  
                  <div className="space-y-2 overflow-y-auto pr-2 flex-1 no-scrollbar md:scrollbar-default">
                      {activeSchedule?.steps.map((step, index) => {
                          if (index < (status.step - 1)) return null;
                          return (
                              <div key={index} className="flex flex-col gap-3 p-3 bg-zinc-900/50 rounded-lg border border-zinc-800">
                                  <div className="flex justify-between items-center">
                                      <div className="text-sm font-bold text-zinc-300">{t.dashboard.stepHold} {index + 1} {step.type === 'hold' ? t.dashboard.holdSuffix : t.dashboard.tempSuffix}</div>
                                      <div className="text-xs text-zinc-500">{t.dashboard.currentTemp}: {step.target}°C</div>
                                  </div>
                                  <div className="grid grid-cols-4 gap-2">
                                      <button 
                                          onClick={() => submitAddTemp(1, index)}
                                          className="py-2 bg-zinc-800 hover:bg-zinc-700 rounded text-xs font-bold text-white transition-colors border border-zinc-700 active:bg-zinc-600"
                                      >
                                          +1°C
                                      </button>
                                      <button 
                                          onClick={() => submitAddTemp(5, index)}
                                          className="py-2 bg-zinc-800 hover:bg-zinc-700 rounded text-xs font-bold text-white transition-colors border border-zinc-700 active:bg-zinc-600"
                                      >
                                          +5°C
                                      </button>
                                      <button 
                                          onClick={() => submitAddTemp(10, index)}
                                          className="py-2 bg-zinc-800 hover:bg-zinc-700 rounded text-xs font-bold text-white transition-colors border border-zinc-700 active:bg-zinc-600"
                                      >
                                          +10°C
                                      </button>
                                      <button 
                                          onClick={() => submitAddTemp(50, index)}
                                          className="py-2 bg-zinc-800 hover:bg-zinc-700 rounded text-xs font-bold text-white transition-colors border border-zinc-700 active:bg-zinc-600"
                                      >
                                          +50°C
                                      </button>
                                  </div>
                              </div>
                          );
                      })}
                      {activeSchedule?.steps.every((s, i) => i < (status.step - 1)) && (
                          <div className="text-center text-zinc-500 py-4">
                              {t.dashboard.noStepsAvailable}
                          </div>
                      )}
                  </div>
              </div>
          </div>
      )}

      {/* Stat Cards Row */}
      <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-6 gap-3">
        {/* MEGA CARD: Temp & Status */}
        <div className="bg-kiln-card border border-kiln-border rounded-xl p-5 shadow-lg shadow-black/20 relative overflow-hidden md:col-span-2 lg:col-span-4 flex flex-col md:flex-row items-center justify-between group min-h-28 lg:min-h-[12.75rem] gap-3 md:gap-0">
            <div className="relative z-10 text-center md:text-left">
                <div className="text-xs font-bold text-zinc-500 uppercase tracking-widest mb-1">
                    {t.dashboard.currentTemp}
                </div>
                <div className="text-6xl lg:text-7xl font-bold text-white tracking-tighter tabular-nums leading-none">
                    {(status.temp || 0).toFixed(1)}
                    <span className="text-3xl lg:text-4xl text-zinc-600 font-normal ml-1 align-top">°C</span>
                </div>
            </div>
            
            <div className="relative z-10 flex flex-col items-center md:items-end gap-2 text-center md:text-right">
                <div className={`text-2xl lg:text-3xl font-bold tracking-tight ${isIdle ? 'text-zinc-500' : 'text-kiln-accent'}`}>
                    {(t.status as Record<string, string>)[status.status] || status.status}
                </div>
                <div className="text-xs font-medium text-zinc-400 bg-zinc-900/50 px-3 py-1.5 rounded-lg border border-zinc-800 inline-flex items-center gap-2">
                    <span className="text-indigo-400 uppercase tracking-wider">{t.dashboard.setpoint}</span>
                    <span className="text-white font-mono text-lg leading-none">{(status.target || 0).toFixed(0)}°C</span>
                </div>
            </div>
            
            {/* Background Glow */}
            <div className={`absolute -right-10 -top-10 w-64 h-64 bg-kiln-accent/5 rounded-full blur-3xl pointer-events-none transition-opacity duration-700 ${!isIdle ? 'opacity-100' : 'opacity-0'}`} />
        </div>

        <div className="md:hidden md:col-span-2 lg:col-span-2 grid grid-cols-2 gap-3 auto-rows-[6rem]">
          <StatCard 
              title={t.dashboard.timeRemaining} 
              value={formatTimeRemaining(status)} 
              unit="" 
          />
          <StatCard 
              title={t.dashboard.segment.toUpperCase()} 
              value={getSegmentInfo()} 
              unit="" 
              valueClass="text-purple-400"
          />
          <div className="bg-kiln-card border kiln-border rounded-xl p-3 shadow-lg shadow-black/20 min-w-0 h-24 relative overflow-hidden col-span-2 flex items-center gap-3">
            <div className="shrink-0">
                <button
                    onClick={toggleFanPower}
                    disabled={fanBusy}
                    className="p-2 rounded-md"
                    title={isFanEnabled ? 'Fan ON' : 'Fan OFF'}
                >
                    <Wind size={38} className={isFanEnabled ? 'text-emerald-400' : 'text-zinc-500'} />
                </button>
            </div>
            <div className="flex-1 text-right">
                <div className="text-xl font-bold text-white tabular-nums leading-none">{fanSpeed}%</div>
                <div className="text-[10px] text-zinc-500 mt-1">PCB {Number(status.pcbTemp ?? 0).toFixed(1)}°C</div>
            </div>
            <div className="flex flex-row items-center gap-2">
                <button
                    onClick={() => nudgeFanManual(-5)}
                    disabled={fanBusy}
                    className="w-10 h-10 rounded-md bg-zinc-800 text-zinc-200 text-2xl leading-none active:scale-95"
                    title="Decrease fan power"
                >
                    -
                </button>
                <button
                    onClick={() => nudgeFanManual(5)}
                    disabled={fanBusy}
                    className="w-10 h-10 rounded-md bg-zinc-800 text-zinc-200 text-2xl leading-none active:scale-95"
                    title="Increase fan power"
                >
                    +
                </button>
            </div>
          </div>
        </div>
      </div>

      {/* Autotune status */}
      {(status.autotune?.active || status.status === 'TUNING') && (
        <div className="bg-kiln-card border border-kiln-border rounded-xl p-4 shadow-lg shadow-black/20 flex flex-col md:flex-row gap-3 md:items-center md:justify-between">
            <div className="text-sm text-zinc-300">
                <span className="font-bold text-white">Autotune</span>
                <span className="text-zinc-500"> • </span>
                <span>Cycles: <span className="font-mono text-white">{status.autotune?.cycles ?? 0}/6</span></span>
                <span className="text-zinc-500"> • </span>
                <span>Ku: <span className="font-mono text-white">{(status.autotune?.ku ?? 0).toFixed(2)}</span></span>
                <span className="text-zinc-500"> • </span>
                <span>Pu: <span className="font-mono text-white">{(status.autotune?.pu_s ?? 0).toFixed(0)}s</span></span>
                <span className="text-zinc-500"> • </span>
                <span>PID: <span className="font-mono text-white">{(status.autotune?.kp ?? 0).toFixed(2)}/{(status.autotune?.ki ?? 0).toFixed(3)}/{(status.autotune?.kd ?? 0).toFixed(2)}</span></span>
            </div>
            <button
                onClick={handleAutotuneStop}
                className="px-4 py-2 bg-red-500 text-white rounded-lg hover:bg-red-600 transition-colors text-sm font-bold whitespace-nowrap"
            >
                STOP Autotune
            </button>
        </div>
      )}

      {/* Main Chart Section */}
      <div className="flex-1 bg-kiln-card border border-kiln-border rounded-xl shadow-lg shadow-black/20 flex flex-col overflow-hidden min-h-[400px]">
        {/* Header */}
        <div className="p-5 border-b border-kiln-border flex flex-col md:flex-row justify-between items-center gap-4 bg-kiln-card/50">
            <div className="flex items-center gap-3 w-full md:w-auto justify-between md:justify-start">
                <h2 className="text-lg font-bold text-white flex items-center gap-2">
                    {t.dashboard.firingProfile}
                </h2>
                <div className="flex items-center gap-3">
                    <span className={`text-sm font-bold ${status.pcbTemp && status.pcbTemp > 50 ? 'text-red-500 animate-pulse' : 'text-zinc-500'}`}>
                        PCB: {status.pcbTemp?.toFixed(1)}°C
                    </span>
                    <div className={`px-3 py-1 rounded-full text-xs font-bold ${!isIdle ? 'bg-red-500/20 text-red-400 animate-pulse' : 'bg-zinc-800 text-zinc-400'}`}>
                        {status.status}
                    </div>
                </div>
            </div>
            
            <div className="flex items-center gap-3 w-full md:w-auto overflow-x-auto pb-2 md:pb-0 hide-scrollbar">
                 {/* On-the-fly controls */}
                 {!isIdle && (
                    <div className="hidden md:flex items-center gap-1.5 mr-2 border-r border-zinc-700 pr-2 shrink-0">
                        <button onClick={handleAddTimeClick} className="px-2.5 py-1.5 bg-zinc-800 hover:bg-zinc-700 rounded-lg text-xs font-bold text-zinc-300 flex items-center gap-1.5 transition-colors border border-zinc-700 whitespace-nowrap">
                            <Clock size={13} /> {t.dashboard.addTime}
                        </button>
                        <button onClick={handleAddTempClick} className="px-2.5 py-1.5 bg-zinc-800 hover:bg-zinc-700 rounded-lg text-xs font-bold text-zinc-300 flex items-center gap-1.5 transition-colors border border-zinc-700 whitespace-nowrap">
                            <Thermometer size={13} /> {t.dashboard.addTemp}
                        </button>
                        <button onClick={handleSkip} className="px-2.5 py-1.5 bg-zinc-800 hover:bg-zinc-700 rounded-lg text-xs font-bold text-zinc-300 flex items-center gap-1.5 transition-colors border border-zinc-700 whitespace-nowrap">
                            <Play size={13} className="fill-current" /> {t.dashboard.skip}
                        </button>
                    </div>
                 )}

                 <div className="relative shrink-0 hidden md:block">
                     {isIdle ? (
                         <button 
                            onClick={handleStartFiring}
                            disabled={!activeSchedule}
                            className={`flex items-center gap-2 px-5 py-2 rounded-lg transition-colors text-sm font-bold whitespace-nowrap ${activeSchedule ? 'bg-kiln-accent text-black hover:bg-emerald-400 shadow-[0_0_15px_rgba(16,185,129,0.2)]' : 'bg-zinc-800 text-zinc-500 cursor-not-allowed'}`}
                         >
                            <Play size={16} fill={activeSchedule ? "black" : "currentColor"} /> 
                            {activeSchedule ? t.dashboard.startFiring : t.dashboard.selectSchedule}
                         </button>
                     ) : (
                         <button 
                            onClick={handleStopFiring}
                            className="flex items-center gap-2 px-5 py-2 bg-red-500 text-white rounded-lg hover:bg-red-600 transition-colors text-sm font-bold shadow-[0_0_15px_rgba(239,68,68,0.2)] whitespace-nowrap"
                         >
                            <Square size={16} fill="white" /> {t.dashboard.stopFiring}
                         </button>
                     )}
                 </div>
            </div>
        </div>

        {/* Chart */}
        <div className="flex-1 p-6 relative">
            <Line options={{
                ...chartOptions,
                scales: {
                    x: {
                        type: 'linear',
                        grid: { color: '#27272a' },
                        ticks: { color: '#71717a', callback: (v) => v + 'h' }
                    },
                    y: {
                        grid: { color: '#27272a' },
                        ticks: { color: '#71717a' }
                    }
                }
            }} data={{
                datasets: [
                    {
                        label: 'Target Profile',
                        data: getTargetProfile(activeSchedule),
                        borderColor: '#6366f1', // Indigo (Target)
                        borderDash: [5, 5],
                        pointRadius: 0,
                        tension: 0,
                        fill: false
                    },
                    {
                        label: 'Actual Temp',
                        data: history,
                        borderColor: '#10b981', // Emerald
                        backgroundColor: 'rgba(16, 185, 129, 0.1)',
                        tension: 0.4,
                        fill: true
                    }
                ]
            }} />
            
            <div className="absolute bottom-6 left-16 text-xs text-orange-500 font-medium">
                {t.dashboard.setpoint}
            </div>
        </div>
      </div>
      {/* Schedule Selection Modal (Mobile) - MOVED TO ROOT */}
      {isScheduleMenuOpen && (
          <div className="fixed inset-0 z-[200] bg-black/95 backdrop-blur-sm flex flex-col p-4 animate-in slide-in-from-bottom-10 duration-200">
              <div className="flex justify-between items-center mb-6">
                  <h2 className="text-xl font-bold text-white flex items-center gap-2">
                      <Sliders size={24} className="text-kiln-accent" />
                      {t.schedules.library}
                  </h2>
                  <div className="flex gap-2">
                      <button onClick={refreshSchedules} className="p-2 bg-zinc-800 rounded-full text-zinc-400 hover:text-white">
                          <Activity size={20} />
                      </button>
                      <button onClick={() => setIsScheduleMenuOpen(false)} className="p-2 bg-zinc-800 rounded-full text-zinc-400 hover:text-white">
                          <Check size={24} />
                      </button>
                  </div>
              </div>
              <div className="flex-1 overflow-y-auto space-y-3 no-scrollbar md:scrollbar-default">
                  {(!schedules || schedules.length === 0) ? (
                      <div className="text-center p-8 text-zinc-500 flex flex-col items-center gap-2">
                          <Sliders size={48} className="opacity-20" />
                          <div className="font-bold">{t.schedules.selectToEdit}</div>
                          <div className="text-xs">{t.schedules.orCreate}</div>
                      </div>
                  ) : (
                      schedules.map(s => (
                          <button
                              key={s.id}
                              onClick={() => { setSelectedScheduleId(s.id); setIsScheduleMenuOpen(false); }}
                              className={`w-full p-4 rounded-xl text-left border transition-all ${selectedScheduleId === s.id ? 'bg-zinc-800 border-kiln-accent' : 'bg-zinc-900/50 border-zinc-800'}`}
                          >
                              <div className="font-bold text-white text-lg">{s.name}</div>
                              <div className="text-sm text-zinc-500">{getScheduleStepCount(s)} segments</div>
                          </button>
                      ))
                  )}
              </div>
          </div>
      )}
      </div>
    </div>
  );
};

export default Dashboard;



