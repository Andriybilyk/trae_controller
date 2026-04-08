import React, { useState, useEffect } from 'react';
import { Plus, Trash2, Save, Play } from 'lucide-react';
import { useLanguage } from '../contexts/LanguageContext';
import { useSchedules, Schedule, Step } from '../contexts/SchedulesContext';
import { Line } from 'react-chartjs-2';
import { Chart as ChartJS, CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler } from 'chart.js';
import toast from 'react-hot-toast';
import { useNavigate } from 'react-router-dom';

const formatHourMinuteLabel = (hoursValue: number, hourSuffix: string, minSuffix: string) => {
  if (!Number.isFinite(hoursValue)) return '';
  const totalMinutes = Math.max(0, Math.round(hoursValue * 60));
  const hours = Math.floor(totalMinutes / 60);
  const minutes = totalMinutes % 60;
  if (minutes === 0) return `${hours}${hourSuffix}`;
  return `${hours}${hourSuffix} ${minutes}${minSuffix}`;
};

const editorBreakpointLabelsPlugin = {
  id: 'editorBreakpointLabels',
  afterDraw: (chart: any, _args: any, pluginOptions: any) => {
    const datasetIndex = Number(pluginOptions?.datasetIndex ?? 0);
    const dataset = chart?.data?.datasets?.[datasetIndex];
    const points = Array.isArray(dataset?.data) ? dataset.data : [];
    const chartArea = chart?.chartArea;
    const xScale = chart?.scales?.x;
    const yScale = chart?.scales?.y;

    if (!chartArea || !xScale || !yScale || !points.length) return;

    const ctx = chart.ctx;

    ctx.save();
    ctx.setLineDash([5, 4]);
    ctx.lineWidth = 1.25;
    ctx.strokeStyle = 'rgba(129, 140, 248, 0.75)';
    ctx.fillStyle = 'rgba(226, 232, 240, 0.98)';
    ctx.font = '700 12px Inter, system-ui, sans-serif';

    for (let i = 1; i < points.length; i += 1) {
      const point = points[i];
      if (!point || typeof point.x !== 'number' || typeof point.y !== 'number') continue;

      const x = xScale.getPixelForValue(point.x);
      const y = yScale.getPixelForValue(point.y);
      if (x < chartArea.left || x > chartArea.right || y < chartArea.top || y > chartArea.bottom) continue;

      const lineBottomY = chartArea.bottom - 14;
      const lineTopY = Math.min(Math.max(y + 6, chartArea.top + 6), lineBottomY - 4);

      ctx.beginPath();
      ctx.moveTo(x, lineTopY);
      ctx.lineTo(x, lineBottomY);
      ctx.stroke();

      const tempLabel = `${Math.round(point.y)}°C`;
      const labelY = Math.min(Math.max(y - 10, chartArea.top + 10), chartArea.bottom - 18);
      ctx.textBaseline = 'middle';
      if (x - 10 < chartArea.left + 24) {
        ctx.textAlign = 'left';
        ctx.fillText(tempLabel, x + 10, labelY);
      } else {
        ctx.textAlign = 'right';
        ctx.fillText(tempLabel, x - 10, labelY);
      }
    }

    ctx.restore();
  }
};

const dynamicXAxisTicksPlugin = {
  id: 'dynamicXAxisTicks',
  afterBuildTicks: (_chart: any, args: any, pluginOptions: any) => {
    const scale = args?.scale;
    if (!scale || scale.id !== 'x') return;
    const values = Array.isArray(pluginOptions?.values) ? pluginOptions.values : [];
    const safe = values
      .map((v: any) => Number(v))
      .filter((v: number) => Number.isFinite(v) && v >= 0)
      .sort((a: number, b: number) => a - b);
    const tickValues = safe.length > 0 ? Array.from(new Set(safe)) : [0];
    scale.ticks = tickValues.map((v: number) => ({ value: v }));
  }
};

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler);

// Frontend Editor Type (Merged Segment)
interface EditorStep {
    id: number;
    rate: number | "";
    target: number | "";
    holdTime: number | "";
    fan?: boolean;
}

const ScheduleEditor = () => {
  const { t } = useLanguage();
  const navigate = useNavigate();
  const fixDegree = (v: string) => v.replace(/\uFFFD/g, '°');
  const { schedules, isLoading, saveSchedule, deleteSchedule, getScheduleDetails } = useSchedules();
  
  const [selectedScheduleId, setSelectedScheduleId] = useState<string | null>(null);
  const [editorSteps, setEditorSteps] = useState<EditorStep[]>([]);
  const [scheduleName, setScheduleName] = useState("");
  const [showDeleteConfirm, setShowDeleteConfirm] = useState(false);
  const [showMobileLibrary, setShowMobileLibrary] = useState(true);

  // Derived selected schedule object from ID
  const selectedSchedule = schedules.find(s => s.id === selectedScheduleId) || null;

  // Effect: Handle Selection & Loading
  useEffect(() => {
    if (selectedSchedule) {
        setScheduleName(selectedSchedule.name);
        
        // If steps are missing, load them
        if (!selectedSchedule.steps || selectedSchedule.steps.length === 0) {
             getScheduleDetails(selectedSchedule);
             // We don't load into editor yet, wait for update
        } else {
             loadScheduleIntoEditor(selectedSchedule);
        }
    } else {
        // Auto-select first if available and nothing selected
        if (schedules.length > 0 && !selectedScheduleId) {
             setSelectedScheduleId(schedules[0].id);
        } else {
            setEditorSteps([]);
            setScheduleName("");
        }
    }
  }, [selectedScheduleId, schedules]); // Re-run when schedules list updates (e.g. details loaded)

  // Convert Backend Steps -> Editor Segments
  const loadScheduleIntoEditor = (schedule: Schedule) => {
      const newSteps: EditorStep[] = [];
      if (schedule && schedule.steps) {
        let i = 0;
        while(i < schedule.steps.length) {
            const step = schedule.steps[i];
            
            let rate = step.rate || 0;
            let target = step.target || 0;
            let holdTime = 0;
            let fan = step.fan;

            if (step.type === 'hold') {
                holdTime = step.holdTime || 0;
                target = step.target; 
            } else {
                // Ramp
                rate = step.rate || 100;
                target = step.target;
                
                // Check if next is HOLD with same target
                if (i + 1 < schedule.steps.length) {
                    const next = schedule.steps[i+1];
                    if (next.type === 'hold' && Math.abs(next.target - target) < 1) {
                        holdTime = next.holdTime || 0;
                        i++; // Consume next step
                    }
                }
            }

            newSteps.push({
                id: Date.now() + Math.random(), // New UI ID
                rate,
                target,
                holdTime,
                fan
            });
            i++;
        }
      }
      setEditorSteps(newSteps);
  };

  // Convert Editor Segments -> Backend Steps & Save
  const handleSave = async () => {
      if (!selectedSchedule) return;

      const backendSteps: Step[] = [];
      editorSteps.forEach(s => {
          const rate = s.rate === "" ? 0 : Number(s.rate);
          const target = s.target === "" ? 0 : Number(s.target);
          const holdTime = s.holdTime === "" ? 0 : Number(s.holdTime);

          // 1. Ramp part
          backendSteps.push({
              id: Date.now() + Math.random(),
              type: 'ramp',
              rate: rate,
              target: target,
              fan: s.fan
          });

          // 2. Hold part (if any)
          if (holdTime > 0) {
              backendSteps.push({
                  id: Date.now() + Math.random() + 1,
                  type: 'hold',
                  holdTime: holdTime,
                  target: target,
                  fan: s.fan
              });
          }
      });

      const safeName = scheduleName.replace(/ /g, "_").replace(/\//g, "");
      
      const updatedSchedule: Schedule = {
          ...selectedSchedule,
          id: safeName, // Ensure ID updates if name changes
          name: safeName,
          steps: backendSteps,
          stepsCount: backendSteps.length
      };

      const success = await saveSchedule(updatedSchedule);
      if (success) {
          toast.success(t.schedules.saveBtn + " OK!");
          // If name changed, the ID changed, so we need to select the new ID
          if (safeName !== selectedSchedule.id) {
              setSelectedScheduleId(safeName);
          }
      }
  };

  const handleSelectSchedule = (s: Schedule) => {
      setSelectedScheduleId(s.id);
      localStorage.setItem('kiln:selectedScheduleId', s.id);
      setShowMobileLibrary(false);
  };

  const handlePlaySchedule = (s: Schedule) => {
      setSelectedScheduleId(s.id);
      localStorage.setItem('kiln:selectedScheduleId', s.id);
      navigate('/');
  };

  const handleAddSegment = () => {
      const lastTarget = editorSteps.length > 0 ? Number(editorSteps[editorSteps.length-1].target) : 25;
      const newStep: EditorStep = {
          id: Date.now() + Math.random(),
          rate: 100,
          target: lastTarget + 100,
          holdTime: 0,
          fan: false
      };
      setEditorSteps([...editorSteps, newStep]);
  };

  const handleUpdateEditorStep = (id: number, field: keyof EditorStep, value: any) => {
      if (value === "") {
          setEditorSteps(editorSteps.map(s => s.id === id ? { ...s, [field]: "" } : s));
          return;
      }
      let numVal = Number(value);
      
      // Enforce Limits
      if (field === 'rate') { if (numVal < 0) numVal = 0; if (numVal > 9999) numVal = 9999; }
      if (field === 'target') { if (numVal < 0) numVal = 0; if (numVal > 1320) numVal = 1320; }
      if (field === 'holdTime') { if (numVal < 0) numVal = 0; if (numVal > 9999) numVal = 9999; }

      setEditorSteps(editorSteps.map(s => s.id === id ? { ...s, [field]: field === 'fan' ? value : numVal } : s));
  };

  const handleRemoveEditorStep = (id: number) => {
      setEditorSteps(editorSteps.filter(s => s.id !== id));
  };

  const handleDelete = async () => {
      if (!selectedSchedule) return;
      setShowDeleteConfirm(true);
  };

  const confirmDelete = async () => {
      if (!selectedSchedule) return;
      const success = await deleteSchedule(selectedSchedule);
      if (success) {
          setSelectedScheduleId(null);
          toast.success("Deleted");
      }
      setShowDeleteConfirm(false);
  };

  const handleAddSchedule = async () => {
      const baseName = t.schedules.newSchedule.replace(/ /g, "_"); 
      let counter = 1;
      let newName = `${baseName}_${counter}`;
      
      while(schedules.find(s => s.name === newName)) {
          counter++;
          newName = `${baseName}_${counter}`;
      }

      const newSchedule: Schedule = {
          id: newName,
          name: newName,
          type: "Custom",
          steps: [],
          stepsCount: 0
      };
      
      // We don't save to backend immediately, just add to local context list?
    // No, Context is source of truth. We must save to backend to persist.
    // Or we can add to context state temporarily?
    // Let's save empty schedule to backend to keep it simple and consistent.
    // We pass steps: [] explicitly.
    const success = await saveSchedule(newSchedule);
    if (success) {
        setSelectedScheduleId(newSchedule.id);
        setShowMobileLibrary(false);
        // Force editor clear if for some reason effect didn't catch it
        setEditorSteps([]);
        setScheduleName(newSchedule.name);
    }
  };

  // Chart Data from Editor Steps
  const getChartData = () => {
      const baseTemp = 0;
      const dataPoints = [{x: 0, y: baseTemp}];
      let currentTime = 0;
      let currentTemp = baseTemp;

      editorSteps.forEach(step => {
          const rate = step.rate === "" ? 0 : Number(step.rate);
          const target = step.target === "" ? 0 : Number(step.target);
          const holdMinutes = step.holdTime === "" ? 0 : Number(step.holdTime);

          const diff = Math.abs(target - currentTemp);
          const rampHours = diff / (rate || 100);
          currentTime += rampHours;
          currentTemp = target;
          dataPoints.push({x: currentTime, y: currentTemp});

          if (holdMinutes > 0) {
              currentTime += holdMinutes / 60;
              dataPoints.push({x: currentTime, y: currentTemp});
          }
      });

      return {
          datasets: [{
              label: 'Profile',
              data: dataPoints,
              borderColor: '#10b981',
              backgroundColor: 'rgba(16, 185, 129, 0.1)',
              fill: true,
              pointRadius: 4,
              pointHoverRadius: 5,
              pointBorderWidth: 1,
              pointBackgroundColor: '#0f1115',
              pointBorderColor: '#818cf8',
              tension: 0.1
          }]
      };
  };
  const chartData = getChartData();
  const editorPoints = (chartData.datasets[0].data || []) as { x: number; y: number }[];
  const editorTickValues = Array.from(
      new Set(
          editorPoints
              .map((p) => Number((Number(p.x) || 0).toFixed(4)))
              .filter((v) => Number.isFinite(v) && v >= 0)
      )
  ).sort((a, b) => a - b);
  const editorXMax = editorTickValues.length > 0 ? editorTickValues[editorTickValues.length - 1] : 0;

  const chartOptions = {
    responsive: true,
    maintainAspectRatio: false,
    scales: {
        x: {
            type: 'linear' as const,
            min: 0,
            max: editorXMax,
            grid: { color: '#27272a' },
            ticks: { color: '#71717a', autoSkip: false, maxRotation: 0, callback: (v: any) => formatHourMinuteLabel(Number(v), t.dashboard.hourSuffix, t.dashboard.minSuffix) }
        },
        y: {
            grid: { color: '#27272a' },
            ticks: { color: '#71717a' }
        }
    },
    plugins: {
        legend: { display: false },
        editorBreakpointLabels: {
            datasetIndex: 0,
            hourSuffix: t.dashboard.hourSuffix,
            minSuffix: t.dashboard.minSuffix
        },
        dynamicXAxisTicks: {
            values: editorTickValues
        }
    }
  };

  return (
    <div className="flex h-full gap-6 p-4 md:p-6 max-w-[1600px] mx-auto overflow-hidden">
      
      {/* Sidebar */}
      <div className={`${showMobileLibrary ? 'flex' : 'hidden'} md:flex w-full md:w-80 flex-col gap-4 shrink-0 bg-kiln-card border border-kiln-border rounded-xl p-4 shadow-lg overflow-hidden`}>
          <div className="flex justify-between items-center mb-2">
            <h2 className="text-lg font-bold text-white">{t.schedules.library}</h2>
            <button onClick={handleAddSchedule} className="p-2 bg-zinc-800 rounded hover:bg-zinc-700 text-zinc-400 hover:text-white transition flex items-center gap-2">
                <Plus size={18}/>
                <span className="md:hidden text-sm font-bold">New</span>
            </button>
          </div>
          <div className="flex-1 overflow-y-auto space-y-2 no-scrollbar md:scrollbar-default">
              {schedules && schedules.map(s => (
                  <div key={s.id} onClick={() => handleSelectSchedule(s)} className={`p-4 rounded-xl cursor-pointer border ${selectedScheduleId === s.id ? 'bg-zinc-800 border-kiln-accent' : 'bg-zinc-900/50 border-zinc-800 hover:border-zinc-600'}`}>
                      <div className="flex items-center justify-between gap-3">
                          <div className="min-w-0">
                              <div className="font-bold text-sm text-white truncate">{s.name}</div>
                              <div className="text-xs text-zinc-500">{s.steps ? s.steps.length : (s.stepsCount || 0)} {t.schedules.steps}</div>
                          </div>
                          <button
                              onClick={(e) => {
                                  e.stopPropagation();
                                  handlePlaySchedule(s);
                              }}
                              className="w-11 h-11 rounded-full bg-kiln-accent text-black flex items-center justify-center hover:bg-emerald-400 transition-colors shadow-[0_0_12px_rgba(16,185,129,0.35)] shrink-0"
                              title={t.dashboard.startFiring || 'Open on dashboard'}
                          >
                              <Play size={18} className="fill-black ml-0.5" />
                          </button>
                      </div>
                  </div>
              ))}
          </div>
      </div>

      {/* Editor */}
      <div className={`${!showMobileLibrary ? 'flex' : 'hidden'} md:flex flex-1 flex-col h-full bg-kiln-bg rounded-xl overflow-hidden`}>
          {selectedSchedule ? (
              <div className="flex flex-col h-full">
                <div className="mb-4 flex flex-col md:flex-row justify-between items-center gap-2 shrink-0 px-1">
                    <div className="w-full flex items-center gap-2">
                        <button onClick={() => setShowMobileLibrary(true)} className="md:hidden p-2 -ml-2 text-zinc-400 hover:text-white">
                            Back
                        </button>
                        <input className="text-xl md:text-3xl font-bold text-white bg-transparent border-none p-0 w-full focus:ring-0 truncate" value={scheduleName} onChange={e => setScheduleName(e.target.value)} />
                    </div>
                    
                    <div className="flex gap-2 w-full md:w-auto">
                        <button onClick={handleSave} className="flex-1 md:flex-none flex items-center justify-center gap-2 bg-kiln-accent hover:bg-emerald-400 text-black px-3 py-2 rounded-lg font-bold text-xs shadow-lg shadow-emerald-900/20 whitespace-nowrap">
                            <Save size={14} /> {t.schedules.saveBtn}
                        </button>
                        <button onClick={handleDelete} className="p-2 bg-red-500/10 text-red-500 rounded-lg hover:bg-red-500/20"><Trash2 size={16}/></button>
                    </div>
                </div>

                {/* Header (Desktop Only) */}
                <div className="hidden md:flex gap-6 mb-2 px-6 py-3 bg-zinc-800/50 rounded-lg text-xs font-bold text-zinc-400 uppercase tracking-widest border border-zinc-800 shrink-0">
                    <div className="w-10 text-center">#</div>
                    <div className="flex-1 grid grid-cols-9 gap-6">
                        <div className="col-span-3 text-center">{t.schedules.rate}</div>
                        <div className="col-span-3 text-center">{t.schedules.targetTemp}</div>
                        <div className="col-span-3 text-center">{t.schedules.time}</div>
                    </div>
                    <div className="w-10"></div>
                </div>

                {/* List */}
                <div className={`flex-1 overflow-y-auto space-y-3 pr-2 pb-20 md:pb-0 min-h-0 no-scrollbar md:scrollbar-default ${isLoading ? 'opacity-50 pointer-events-none' : ''}`}>
                    {editorSteps.map((step, index) => (
                        <div key={step.id} className="flex flex-col md:flex-row items-center gap-4 md:gap-6 p-4 bg-kiln-card border border-kiln-border rounded-xl hover:border-zinc-700 transition-colors">
                            {/* Mobile Step Header */}
                            <div className="md:hidden flex justify-between items-center w-full border-b border-zinc-800 pb-2 mb-2">
                                <span className="font-bold text-zinc-400 text-sm">{t.schedules.step} {index + 1}</span>
                                <button onClick={() => handleRemoveEditorStep(step.id)} className="text-red-500 p-1"><Trash2 size={16}/></button>
                            </div>

                            <div className="hidden md:flex w-10 justify-center text-zinc-500 font-mono font-bold text-lg">{index + 1}</div>
                            
                            <div className="flex-1 grid grid-cols-2 md:grid-cols-9 gap-3 md:gap-6 items-center w-full">
                                <div className="col-span-1 md:col-span-3 relative">
                                    <label className="md:hidden text-[10px] font-bold text-zinc-500 uppercase mb-1 block">{t.schedules.rate}</label>
                                    <input type="number" min="0" max="9999" className="bg-zinc-900/50 border-zinc-800 rounded-lg pl-3 pr-14 py-2 text-white font-mono w-full focus:border-kiln-accent text-center text-lg" 
                                        value={step.rate} onChange={e => handleUpdateEditorStep(step.id, 'rate', e.target.value)} placeholder="FULL" />
                                    <span className="absolute right-3 bottom-2.5 text-zinc-600 text-[10px] font-bold pointer-events-none">{fixDegree(t.schedules.rateUnit).replace(/[()]/g, '')}</span>
                                </div>
                                <div className="col-span-1 md:col-span-3 relative">
                                    <label className="md:hidden text-[10px] font-bold text-zinc-500 uppercase mb-1 block">{t.schedules.targetTemp}</label>
                                    <input type="number" min="0" max="1320" className="bg-zinc-900/50 border-zinc-800 rounded-lg pl-3 pr-8 py-2 text-white font-mono w-full focus:border-kiln-accent text-center text-lg font-bold" 
                                        value={step.target} onChange={e => handleUpdateEditorStep(step.id, 'target', e.target.value)} />
                                    <span className="absolute right-3 bottom-2.5 text-zinc-600 text-[10px] font-bold pointer-events-none">{fixDegree(t.schedules.tempUnit).replace(/[()]/g, '')}</span>
                                </div>
                                <div className="col-span-2 md:col-span-3 relative">
                                    <label className="md:hidden text-[10px] font-bold text-zinc-500 uppercase mb-1 block">{t.schedules.time}</label>
                                    <input type="number" min="0" max="9999" className="bg-zinc-900/50 border-zinc-800 rounded-lg pl-3 pr-8 py-2 text-white font-mono w-full focus:border-kiln-accent text-center text-lg" 
                                        value={step.holdTime} onChange={e => handleUpdateEditorStep(step.id, 'holdTime', e.target.value)} />
                                    <span className="absolute right-3 bottom-2.5 text-zinc-600 text-[10px] font-bold pointer-events-none">{t.schedules.timeUnit.replace(/[()]/g, '')}</span>
                                </div>
                            </div>

                            <button onClick={() => handleRemoveEditorStep(step.id)} className="hidden md:block w-10 p-2 text-zinc-600 hover:text-red-500"><Trash2 size={18}/></button>
                        </div>
                    ))}
                    
                    <button onClick={handleAddSegment} className="w-full py-4 border-2 border-dashed border-zinc-800 rounded-xl text-zinc-500 hover:text-kiln-accent hover:border-kiln-accent hover:bg-kiln-accent/5 transition-all font-bold uppercase tracking-wider flex items-center justify-center gap-2">
                        <Plus size={20} /> {t.schedules.addSegment}
                    </button>

                    {/* Mobile Chart */}
                    <div className="md:hidden mt-6 h-48 bg-kiln-card border border-kiln-border rounded-xl p-4 shrink-0">
                        <Line data={chartData} options={chartOptions} plugins={[editorBreakpointLabelsPlugin, dynamicXAxisTicksPlugin]} />
                    </div>
                </div>

                {/* Desktop Chart */}
                <div className="hidden md:block mb-6 h-48 bg-kiln-card border border-kiln-border rounded-xl p-4 shrink-0 md:order-first">
                    <Line data={chartData} options={chartOptions} plugins={[editorBreakpointLabelsPlugin, dynamicXAxisTicksPlugin]} />
                </div>
              </div>
          ) : (
              <div className="flex-1 flex items-center justify-center text-zinc-500">{t.schedules.selectToEdit}</div>
          )}
      </div>
      {/* Delete Confirmation Modal */}
      {showDeleteConfirm && (
          <div className="absolute inset-0 z-[110] bg-black/80 backdrop-blur-sm flex items-center justify-center p-4">
              <div className="bg-kiln-card border border-red-500/50 rounded-2xl p-8 max-w-md w-full shadow-[0_0_50px_rgba(239,68,68,0.2)] text-center animate-in fade-in zoom-in duration-200">
                  <div className="w-20 h-20 bg-red-500/10 rounded-full flex items-center justify-center mx-auto mb-6">
                      <Trash2 size={40} className="text-red-500" />
                  </div>
                  
                  <h3 className="text-2xl font-bold text-white mb-2">{t.schedules.deleteConfirmTitle}</h3>
                  <p className="text-zinc-400 mb-8">
                      {t.schedules.deleteConfirmMessage}
                  </p>
                  
                  <div className="flex flex-col gap-3">
                      <button 
                          onClick={confirmDelete}
                          className="w-full py-4 bg-red-600 hover:bg-red-500 text-white rounded-xl font-bold text-lg transition-all shadow-lg shadow-red-900/40"
                      >
                          {t.schedules.deleteConfirmButton}
                      </button>
                      <button 
                          onClick={() => setShowDeleteConfirm(false)}
                          className="w-full py-3 bg-zinc-800 hover:bg-zinc-700 text-zinc-300 rounded-xl font-medium transition-colors"
                      >
                          {t.schedules.cancel}
                      </button>
                  </div>
              </div>
          </div>
      )}
    </div>
  );
};

export default ScheduleEditor;
