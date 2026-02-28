import React, { useState, useEffect } from 'react';
import { Plus, Trash2, Save, GripVertical, Fan, Star, Zap } from 'lucide-react';
import { useLanguage } from '../contexts/LanguageContext';
import { Line } from 'react-chartjs-2';
import { Chart as ChartJS, CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler } from 'chart.js';
import toast from 'react-hot-toast';
import { API_BASE_URL } from '../config';

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler);

// Backend Types
interface Step {
    id: number;
    type: 'ramp' | 'hold';
    target: number;
    rate?: number;
    holdTime?: number;
    fan?: boolean;
}

interface Schedule {
    id: string;
    name: string;
    steps: Step[];
    favorite?: boolean;
    type?: string;
}

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
  const [schedules, setSchedules] = useState<Schedule[]>([]);
  const [selectedSchedule, setSelectedSchedule] = useState<Schedule | null>(null);
  
  // Editor State (Working Copy)
  const [editorSteps, setEditorSteps] = useState<EditorStep[]>([]);
  const [scheduleName, setScheduleName] = useState("");
  const [showMobileLibrary, setShowMobileLibrary] = useState(true);

  // Fetch Schedules on Mount
  useEffect(() => {
    fetchSchedules();
  }, []);

  // When selected schedule changes, load it into editor format
  useEffect(() => {
    if (selectedSchedule) {
        setScheduleName(selectedSchedule.name);
        loadScheduleIntoEditor(selectedSchedule);
    } else {
        setEditorSteps([]);
        setScheduleName("");
    }
  }, [selectedSchedule]);

  const fetchSchedules = async () => {
    try {
        const res = await fetch(`${API_BASE_URL}/schedules`);
        if (res.ok) {
            const data = await res.json();
            if (Array.isArray(data) && data.length > 0) {
                setSchedules(data);
                if (!selectedSchedule) setSelectedSchedule(data[0]);
                return;
            }
        }
    } catch (e) {
        console.error("Failed to fetch schedules", e);
    }
    
    // Fallback Mock
    const mock: Schedule[] = [{ 
        id: '1', name: 'Bisque Fire', steps: [
            { id: 1, type: 'ramp', rate: 100, target: 1000 },
            { id: 2, type: 'hold', holdTime: 10, target: 1000 }
        ]
    }];
    setSchedules(mock);
    if (!selectedSchedule) setSelectedSchedule(mock[0]);
  };

  // Convert Backend Steps -> Editor Segments
  const loadScheduleIntoEditor = (schedule: Schedule) => {
      const newSteps: EditorStep[] = [];
      let i = 0;
      while(i < schedule.steps.length) {
          const step = schedule.steps[i];
          
          let rate = step.rate || 0;
          let target = step.target || 0;
          let holdTime = 0;
          let fan = step.fan;

          if (step.type === 'hold') {
              // Standalone hold? Should be merged with previous if possible, but here we treat as segment with 0 rate (jump?) or just hold
              holdTime = step.holdTime || 0;
              target = step.target; 
              // Try to get target from previous step if missing
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
      setEditorSteps(newSteps);
  };

  // Convert Editor Segments -> Backend Steps
  const saveScheduleFromEditor = async () => {
      if (!selectedSchedule) return;

      const backendSteps: Step[] = [];
      editorSteps.forEach(s => {
          // Validate numbers
          const rate = s.rate === "" ? 0 : Number(s.rate);
          const target = s.target === "" ? 0 : Number(s.target);
          const holdTime = s.holdTime === "" ? 0 : Number(s.holdTime);

          // 1. Ramp part
          backendSteps.push({
              id: Date.now() + Math.random(), // Backend will assign or we assign
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

      const updatedSchedule = {
          ...selectedSchedule,
          name: scheduleName,
          steps: backendSteps
      };

      try {
          const res = await fetch(`${API_BASE_URL}/schedules`, {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify(updatedSchedule)
          });
          
          if (res.ok) {
              const saved = await res.json();
              const updatedList = schedules.map(s => s.id === saved.id ? saved : s);
              // Handle new schedule case
              if (!schedules.find(s => s.id === saved.id)) {
                  setSchedules([...schedules, saved]);
              } else {
                  setSchedules(updatedList);
              }
              // Update selected to reflect saved state (IDs etc)
              setSelectedSchedule(saved); 
              toast.success(t.schedules.save + " OK!");
          }
      } catch (e) {
          toast.error("Error saving");
      }
  };

  const handleSelectSchedule = (s: Schedule) => {
      setSelectedSchedule(s);
      setShowMobileLibrary(false);
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
      // Allow empty string for better UX
      if (value === "") {
          setEditorSteps(editorSteps.map(s => s.id === id ? { ...s, [field]: "" } : s));
          return;
      }

      let numVal = Number(value);
      
      // Enforce Limits
      if (field === 'rate') {
          if (numVal < 0) numVal = 0;
          if (numVal > 9999) numVal = 9999;
      }
      if (field === 'target') {
          if (numVal < 0) numVal = 0;
          if (numVal > 1320) numVal = 1320;
      }
      if (field === 'holdTime') {
          if (numVal < 0) numVal = 0;
          if (numVal > 9999) numVal = 9999;
      }

      setEditorSteps(editorSteps.map(s => s.id === id ? { ...s, [field]: field === 'fan' ? value : numVal } : s));
  };

  const handleRemoveEditorStep = (id: number) => {
      setEditorSteps(editorSteps.filter(s => s.id !== id));
  };

  const handleDeleteSchedule = async () => {
      if (!selectedSchedule || !confirm("Delete schedule?")) return;
      try {
          await fetch(`${API_BASE_URL}/schedules/${selectedSchedule.id}`, { method: 'DELETE' });
          const newList = schedules.filter(s => s.id !== selectedSchedule.id);
          setSchedules(newList);
          setSelectedSchedule(newList.length > 0 ? newList[0] : null);
          toast.success("Deleted");
      } catch(e) { toast.error("Error deleting"); }
  };

  const handleAddSchedule = () => {
      const newSchedule: Schedule = {
          id: Date.now().toString(),
          name: "New Schedule",
          type: "Custom",
          steps: []
      };
      setSchedules([...schedules, newSchedule]);
      setSelectedSchedule(newSchedule);
      setShowMobileLibrary(false);
      // Editor will update via useEffect
  };

  // Chart Data from Editor Steps
  const getChartData = () => {
      const dataPoints = [{x: 0, y: 25}];
      let currentTime = 0;
      let currentTemp = 25;

      editorSteps.forEach(step => {
          const rate = step.rate === "" ? 0 : Number(step.rate);
          const target = step.target === "" ? 0 : Number(step.target);
          const holdTime = step.holdTime === "" ? 0 : Number(step.holdTime);

          // Ramp
          const diff = Math.abs(target - currentTemp);
          const duration = diff / (rate || 100);
          currentTime += duration;
          currentTemp = target;
          dataPoints.push({x: currentTime, y: currentTemp});

          // Hold
          if (holdTime > 0) {
              currentTime += holdTime / 60;
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
              pointRadius: 0,
              tension: 0.1
          }]
      };
  };

  const chartOptions = {
    responsive: true,
    maintainAspectRatio: false,
    scales: {
        x: {
            type: 'linear' as const,
            grid: { color: '#27272a' },
            ticks: { color: '#71717a', callback: (v: any) => v.toFixed(1) + 'h' }
        },
        y: {
            grid: { color: '#27272a' },
            ticks: { color: '#71717a' }
        }
    },
    plugins: { legend: { display: false } }
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
              {schedules.map(s => (
                  <div key={s.id} onClick={() => handleSelectSchedule(s)} className={`p-4 rounded-xl cursor-pointer border ${selectedSchedule?.id === s.id ? 'bg-zinc-800 border-kiln-accent' : 'bg-zinc-900/50 border-zinc-800 hover:border-zinc-600'}`}>
                      <div className="font-bold text-sm text-white truncate">{s.name}</div>
                      <div className="text-xs text-zinc-500">{s.steps.length} steps</div>
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
                            ←
                        </button>
                        <input className="text-xl md:text-3xl font-bold text-white bg-transparent border-none p-0 w-full focus:ring-0 truncate" value={scheduleName} onChange={e => setScheduleName(e.target.value)} />
                    </div>
                    
                    <div className="flex gap-2 w-full md:w-auto">
                        <button onClick={saveScheduleFromEditor} className="flex-1 md:flex-none flex items-center justify-center gap-2 bg-kiln-accent hover:bg-emerald-400 text-black px-3 py-2 rounded-lg font-bold text-xs shadow-lg shadow-emerald-900/20 whitespace-nowrap">
                            <Save size={14} /> {t.schedules.save}
                        </button>
                        <button onClick={handleDeleteSchedule} className="p-2 bg-red-500/10 text-red-500 rounded-lg hover:bg-red-500/20"><Trash2 size={16}/></button>
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
                <div className="flex-1 overflow-y-auto space-y-3 pr-2 pb-20 md:pb-0 min-h-0 no-scrollbar md:scrollbar-default">
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
                                    <span className="absolute right-3 bottom-2.5 text-zinc-600 text-[10px] font-bold pointer-events-none">{t.schedules.rateUnit.replace(/[()]/g, '')}</span>
                                </div>
                                <div className="col-span-1 md:col-span-3 relative">
                                    <label className="md:hidden text-[10px] font-bold text-zinc-500 uppercase mb-1 block">{t.schedules.targetTemp}</label>
                                    <input type="number" min="0" max="1320" className="bg-zinc-900/50 border-zinc-800 rounded-lg pl-3 pr-8 py-2 text-white font-mono w-full focus:border-kiln-accent text-center text-lg font-bold" 
                                        value={step.target} onChange={e => handleUpdateEditorStep(step.id, 'target', e.target.value)} />
                                    <span className="absolute right-3 bottom-2.5 text-zinc-600 text-[10px] font-bold pointer-events-none">{t.schedules.tempUnit.replace(/[()]/g, '')}</span>
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
                        <Line data={getChartData()} options={chartOptions} />
                    </div>
                </div>

                {/* Desktop Chart */}
                <div className="hidden md:block mb-6 h-48 bg-kiln-card border border-kiln-border rounded-xl p-4 shrink-0 md:order-first">
                    <Line data={getChartData()} options={chartOptions} />
                </div>
              </div>
          ) : (
              <div className="flex-1 flex items-center justify-center text-zinc-500">{t.schedules.selectToEdit}</div>
          )}
      </div>
    </div>
  );
};

export default ScheduleEditor;