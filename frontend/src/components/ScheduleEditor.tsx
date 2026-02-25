import React, { useState, useEffect } from 'react';
import { Plus, Trash2, Save, GripVertical, Fan, Star, Zap } from 'lucide-react';
import { useLanguage } from '../contexts/LanguageContext';
import { Line } from 'react-chartjs-2';
import { Chart as ChartJS, CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler } from 'chart.js';

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler);

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

const ScheduleEditor = () => {
  const { t } = useLanguage();
  const [schedules, setSchedules] = useState<Schedule[]>([]);
  const [selectedSchedule, setSelectedSchedule] = useState<Schedule | null>(null);
  
  // Cost Calculator State
  const [power, setPower] = useState(3.0); // kW
  const [tariff, setTariff] = useState(0.15); // $/kWh
  const [estimatedCost, setEstimatedCost] = useState(0);

  // Fetch Schedules on Mount
  useEffect(() => {
    fetchSchedules();
  }, []);

  const fetchSchedules = async () => {
    try {
        const res = await fetch('http://localhost:3000/api/schedules');
        if (res.ok) {
            const data = await res.json();
            if (Array.isArray(data) && data.length > 0) {
                setSchedules(data);
                // If we don't have a selection, select the first one
                if (!selectedSchedule) {
                     setSelectedSchedule(data[0]);
                }
                return;
            }
        }
    } catch (e) {
        console.error("Failed to fetch schedules, using mock data", e);
    }

    // Fallback Mock Data if API fails or is empty
    const mockSchedules: Schedule[] = [
        { 
            id: '1', 
            name: 'Bisque Firing (Cone 04)', 
            favorite: true,
            type: '4 segments',
            steps: [
                { id: 1, type: 'ramp', rate: 100, target: 500, fan: false },
                { id: 2, type: 'ramp', rate: 150, target: 1060, fan: false },
                { id: 3, type: 'hold', holdTime: 10, target: 1060, fan: false },
                { id: 4, type: 'ramp', rate: 200, target: 200, fan: false } // Cooling
            ]
        },
        { 
            id: '2', 
            name: 'Glaze Firing (Cone 6)', 
            favorite: false,
            type: '3 segments',
            steps: [
                { id: 5, type: 'ramp', rate: 200, target: 1220, fan: false },
                { id: 6, type: 'hold', holdTime: 15, target: 1220, fan: false },
                { id: 7, type: 'ramp', rate: 9999, target: 25, fan: true }
            ]
        }
    ];
    setSchedules(mockSchedules);
    if(mockSchedules.length > 0 && !selectedSchedule) setSelectedSchedule(mockSchedules[0]);
  };

  useEffect(() => {
    if (selectedSchedule) {
        calculateCost();
    }
  }, [selectedSchedule, power, tariff]);

  const calculateCost = () => {
    if (!selectedSchedule) return;
    
    let totalHours = 0;
    let currentTemp = 25;

    selectedSchedule.steps.forEach(step => {
        if (step.type === 'ramp') {
            const diff = Math.abs((step.target || 0) - currentTemp);
            const rate = step.rate || 9999;
            const hours = (diff / rate); 
            totalHours += hours; 
            currentTemp = Number(step.target);
        } else {
            const hours = (step.holdTime || 0) / 60;
            totalHours += hours; 
        }
    });

    setEstimatedCost(totalHours * power * tariff);
  };

  const handleUpdateStep = (id: number, field: string, value: any) => {
    if (!selectedSchedule) return;
    const newSteps = selectedSchedule.steps.map(s => 
        s.id === id ? { ...s, [field]: field === 'fan' ? value : Number(value) } : s
    );
    setSelectedSchedule({ ...selectedSchedule, steps: newSteps });
  };

  const handleAddStep = (mode: 'ramp' | 'hold' | 'cool') => {
    if (!selectedSchedule) return;
    let newStep: Step;
    const lastTarget = selectedSchedule.steps.length > 0 ? selectedSchedule.steps[selectedSchedule.steps.length-1].target : 25;

    if (mode === 'ramp') {
        newStep = { id: Date.now(), type: 'ramp', rate: 100, target: lastTarget + 100, fan: false };
    } else if (mode === 'cool') {
        newStep = { id: Date.now(), type: 'ramp', rate: 100, target: 25, fan: false }; 
    } else {
        newStep = { id: Date.now(), type: 'hold', holdTime: 60, target: lastTarget, fan: false };
    }
    setSelectedSchedule({ ...selectedSchedule, steps: [...selectedSchedule.steps, newStep] });
  };

  const handleRemoveStep = (id: number) => {
    if (!selectedSchedule) return;
    setSelectedSchedule({ ...selectedSchedule, steps: selectedSchedule.steps.filter(s => s.id !== id) });
  };

  const handleAddSchedule = () => {
    const newSchedule: Schedule = {
        id: Date.now().toString(),
        name: t.schedules.newSchedule,
        type: 'Custom',
        steps: [
            { id: 1, type: 'ramp', rate: 100, target: 500, fan: false },
            { id: 2, type: 'hold', holdTime: 10, target: 500, fan: false },
            { id: 3, type: 'ramp', rate: 200, target: 25, fan: false }
        ]
    };
    setSchedules([...schedules, newSchedule]);
    setSelectedSchedule(newSchedule);
  };

  const handleSaveSchedule = async () => {
      if (!selectedSchedule) return;
      
      try {
          const res = await fetch('http://localhost:3000/api/schedules', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify(selectedSchedule)
          });
          
          if (res.ok) {
              const saved = await res.json();
              // Update list with saved version (in case ID changed or sanitized)
              const updatedList = schedules.map(s => s.id === selectedSchedule.id ? saved : s);
              // If it was new (not in list yet?), add it. 
              // But handleAddSchedule adds to list first. 
              // Check if we need to append?
              const exists = schedules.find(s => s.id === saved.id);
              if (!exists) {
                  setSchedules([...schedules, saved]);
              } else {
                  setSchedules(updatedList);
              }
              alert(t.schedules.save + " OK!");
          }
      } catch (e) {
          alert("Error saving schedule");
      }
  };

  const handleDeleteSchedule = async () => {
      if (!selectedSchedule) return;
      if (!confirm("Are you sure?")) return;

      try {
          await fetch(`http://localhost:3001/api/schedules/${selectedSchedule.id}`, {
              method: 'DELETE'
          });
          
          const newList = schedules.filter(s => s.id !== selectedSchedule.id);
          setSchedules(newList);
          setSelectedSchedule(newList.length > 0 ? newList[0] : null);
      } catch (e) {
          alert("Error deleting schedule");
      }
  };

  const getBadge = (step: Step, index: number) => {
      const prevTarget = index > 0 ? selectedSchedule!.steps[index-1].target : 25;
      
      if (step.type === 'hold') {
          return <span className="px-3 py-1 rounded bg-blue-500/10 text-blue-400 text-[10px] font-bold uppercase tracking-wider border border-blue-500/20 w-16 text-center">{t.status.HOLD}</span>;
      }
      if (step.type === 'ramp') {
          if (step.target < prevTarget) {
            return <span className="px-3 py-1 rounded bg-emerald-500/10 text-emerald-400 text-[10px] font-bold uppercase tracking-wider border border-emerald-500/20 w-16 text-center">{t.status.COOL}</span>;
          }
          return <span className="px-3 py-1 rounded bg-amber-500/10 text-amber-500 text-[10px] font-bold uppercase tracking-wider border border-amber-500/20 w-16 text-center">{t.status.RAMP}</span>;
      }
      return null;
  };

  const getChartData = () => {
      if (!selectedSchedule) return { datasets: [] };
      
      const dataPoints: {x: number, y: number}[] = [{x: 0, y: 25}];
      let currentTime = 0;
      let currentTemp = 25;
      
      selectedSchedule.steps.forEach(step => {
          if (step.type === 'ramp') {
              const diff = Math.abs((step.target || 0) - currentTemp);
              const rate = step.rate || 100;
              const duration = diff / rate;
              currentTime += duration;
              currentTemp = Number(step.target);
              dataPoints.push({x: currentTime, y: currentTemp});
          } else {
              const duration = (step.holdTime || 0) / 60;
              currentTime += duration;
              dataPoints.push({x: currentTime, y: currentTemp});
          }
      });
      
      return {
          datasets: [{
              label: 'Temperature',
              data: dataPoints,
              borderColor: '#10b981', // Emerald-500
              backgroundColor: 'rgba(16, 185, 129, 0.1)',
              borderWidth: 2,
              fill: true,
              pointRadius: 3,
              pointBackgroundColor: '#10b981',
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
              ticks: { color: '#71717a', callback: (value: any) => value.toFixed(1) + 'h' },
          },
          y: {
              grid: { color: '#27272a' },
              ticks: { color: '#71717a' },
          }
      },
      plugins: {
          legend: { display: false },
          tooltip: {
              mode: 'index' as const,
              intersect: false,
              backgroundColor: '#18181b',
              titleColor: '#fff',
              bodyColor: '#a1a1aa',
              borderColor: '#27272a',
              borderWidth: 1,
              callbacks: {
                  title: (items: any[]) => `Time: ${items[0].parsed.x.toFixed(2)}h`,
                  label: (item: any) => `Temp: ${item.parsed.y}°C`
              }
          }
      }
  };

  return (
    <div className="flex h-full gap-6 p-6 max-w-[1600px] mx-auto overflow-hidden">
      
      {/* Left Sidebar: Library */}
      <div className="w-80 flex flex-col gap-4 shrink-0">
        <div className="flex justify-between items-center px-1">
            <h2 className="text-lg font-bold text-white">{t.schedules.library}</h2>
            <button 
                onClick={handleAddSchedule}
                className="w-8 h-8 flex items-center justify-center bg-zinc-800 hover:bg-zinc-700 rounded-md text-zinc-400 hover:text-white transition-colors"
            >
                <Plus size={18}/>
            </button>
        </div>

        <div className="flex-1 overflow-y-auto space-y-2 pr-2">
            {schedules.map(s => (
                <div 
                    key={s.id}
                    onClick={() => setSelectedSchedule(s)}
                    className={`p-4 rounded-xl cursor-pointer border transition-all relative group ${selectedSchedule?.id === s.id 
                        ? 'bg-kiln-card border-kiln-accent shadow-[0_0_0_1px_rgba(16,185,129,1)]' 
                        : 'bg-kiln-card border-kiln-border hover:border-zinc-600'}`}
                >
                    <div className="font-bold text-sm text-white mb-1 pr-6">{s.name}</div>
                    <div className="text-xs text-zinc-500">{s.steps.length} {t.schedules.segments} • {s.type || 'Custom'}</div>
                    <div className="absolute top-4 right-4 text-zinc-600">
                        {s.favorite ? <Star size={16} fill="#eab308" className="text-yellow-500" /> : <Star size={16} className="group-hover:text-zinc-500" />}
                    </div>
                </div>
            ))}
        </div>
      </div>

      {/* Right Content: Editor */}
      <div className="flex-1 flex flex-col h-full bg-kiln-bg rounded-xl overflow-hidden">
        {selectedSchedule ? (
            <>
                {/* Header */}
                <div className="mb-6 flex justify-between items-end">
                    <div className="relative w-full max-w-xl group">
                        <input 
                            className="text-3xl font-bold text-white bg-transparent border-none p-0 focus:ring-0 w-full placeholder-zinc-700"
                            value={selectedSchedule.name}
                            onChange={(e) => setSelectedSchedule({...selectedSchedule, name: e.target.value})}
                        />
                        <div className="h-0.5 w-full bg-zinc-800 mt-2 group-focus-within:bg-kiln-accent transition-colors"></div>
                    </div>
                    
                    <div className="flex gap-3">
                        <button 
                            onClick={handleSaveSchedule}
                            className="flex items-center gap-2 bg-kiln-accent hover:bg-emerald-400 text-black px-4 py-2 rounded-lg font-bold text-sm transition-colors shadow-lg shadow-emerald-900/20"
                        >
                            <Save size={16} fill="black" strokeWidth={2.5} /> {t.schedules.save}
                        </button>
                        <button 
                            onClick={handleDeleteSchedule}
                            className="w-10 h-10 flex items-center justify-center bg-red-500/10 hover:bg-red-500/20 text-red-500 border border-red-500/20 rounded-lg transition-colors"
                        >
                            <Trash2 size={18}/>
                        </button>
                    </div>
                </div>

                {/* Chart Section */}
                <div className="mb-6 h-48 bg-kiln-card border border-kiln-border rounded-xl p-4 relative">
                    <Line data={getChartData()} options={chartOptions} />
                </div>

                {/* Steps List */}
                <div className="flex-1 overflow-y-auto space-y-3 pr-2 -mr-2">
                    {selectedSchedule.steps.map((step, index) => (
                        <div key={step.id} className="flex items-center gap-4 p-4 bg-kiln-card border border-kiln-border rounded-xl group hover:border-zinc-700 transition-colors shadow-sm">
                            <div className="text-zinc-700 cursor-move hover:text-zinc-500">
                                <GripVertical size={20} />
                            </div>
                            
                            <div className="w-24 flex justify-center">
                                {getBadge(step, index)}
                            </div>

                            <div className="flex-1 grid grid-cols-12 gap-4 items-center">
                                {/* Target Temp */}
                                <div className="col-span-3">
                                    <label className="text-[10px] font-bold text-zinc-600 uppercase mb-1.5 block">{t.schedules.targetTemp}</label>
                                    <input 
                                        type="number" 
                                        className="bg-zinc-900/50 border-zinc-800 focus:border-kiln-accent rounded-lg px-3 py-2 text-white font-mono w-full"
                                        value={step.target}
                                        onChange={(e) => handleUpdateStep(step.id, 'target', e.target.value)}
                                    />
                                </div>
                                
                                {/* Rate or Duration */}
                                {step.type === 'ramp' ? (
                                    <div className="col-span-3">
                                        <label className="text-[10px] font-bold text-zinc-600 uppercase mb-1.5 block">{t.schedules.rate}</label>
                                        <input 
                                            type="number" 
                                            className="bg-zinc-900/50 border-zinc-800 focus:border-kiln-accent rounded-lg px-3 py-2 text-white font-mono w-full"
                                            value={step.rate}
                                            onChange={(e) => handleUpdateStep(step.id, 'rate', e.target.value)}
                                        />
                                    </div>
                                ) : (
                                    <div className="col-span-3">
                                        <label className="text-[10px] font-bold text-zinc-600 uppercase mb-1.5 block">{t.schedules.duration}</label>
                                        <input 
                                            type="number" 
                                            className="bg-zinc-900/50 border-zinc-800 focus:border-kiln-accent rounded-lg px-3 py-2 text-white font-mono w-full"
                                            value={step.holdTime}
                                            onChange={(e) => handleUpdateStep(step.id, 'holdTime', e.target.value)}
                                        />
                                    </div>
                                )}

                                {/* Fan Toggle */}
                                <div className="col-span-3 flex flex-col justify-end h-full pt-6">
                                    <button 
                                        onClick={() => handleUpdateStep(step.id, 'fan', !step.fan)}
                                        className={`flex items-center justify-center gap-2 text-xs font-bold py-2.5 px-3 rounded-lg border transition-all ${
                                            step.fan 
                                            ? 'bg-zinc-100 text-black border-white' 
                                            : 'bg-zinc-900/30 text-zinc-500 border-zinc-800 hover:border-zinc-600'
                                        }`}
                                    >
                                        <Fan size={14} className={step.fan ? 'animate-spin-slow' : ''} /> 
                                        <span>{step.fan ? t.status.ON : t.status.OFF}</span>
                                    </button>
                                </div>
                            </div>

                            <button 
                                className="p-2 text-zinc-600 hover:text-red-400 hover:bg-red-500/10 rounded-lg transition-colors"
                                onClick={() => handleRemoveStep(step.id)}
                            >
                                <Trash2 size={18} />
                            </button>
                        </div>
                    ))}
                </div>

                {/* Footer Actions */}
                <div className="mt-6 grid grid-cols-3 gap-4">
                    <button className="py-3 rounded-xl border border-dashed border-zinc-700 text-zinc-400 hover:text-amber-500 hover:border-amber-500 hover:bg-amber-500/5 transition-all text-sm font-bold uppercase tracking-wider flex items-center justify-center gap-2" onClick={() => handleAddStep('ramp')}>
                        <Plus size={16} /> {t.schedules.addRamp}
                    </button>
                    <button className="py-3 rounded-xl border border-dashed border-zinc-700 text-zinc-400 hover:text-blue-400 hover:border-blue-500 hover:bg-blue-500/5 transition-all text-sm font-bold uppercase tracking-wider flex items-center justify-center gap-2" onClick={() => handleAddStep('hold')}>
                        <Plus size={16} /> {t.schedules.addHold}
                    </button>
                    <button className="py-3 rounded-xl border border-dashed border-zinc-700 text-zinc-400 hover:text-emerald-400 hover:border-emerald-500 hover:bg-emerald-500/5 transition-all text-sm font-bold uppercase tracking-wider flex items-center justify-center gap-2" onClick={() => handleAddStep('cool')}>
                        <Plus size={16} /> {t.schedules.addCool}
                    </button>
                </div>

                {/* Cost / Power Settings */}
                <div className="grid grid-cols-3 gap-4 mt-6 mb-6">
                    <div className="bg-kiln-card border border-kiln-border rounded-xl p-4 flex flex-col gap-2">
                        <label className="text-[10px] font-bold text-zinc-500 uppercase tracking-widest">{t.schedules.power}</label>
                        <input 
                            type="number" 
                            className="bg-zinc-900/50 border-zinc-800 focus:border-kiln-accent rounded-lg px-3 py-2 text-white font-mono"
                            value={power}
                            onChange={e => setPower(Number(e.target.value))}
                        />
                    </div>
                    <div className="bg-kiln-card border border-kiln-border rounded-xl p-4 flex flex-col gap-2">
                        <label className="text-[10px] font-bold text-zinc-500 uppercase tracking-widest">{t.schedules.tariff}</label>
                        <input 
                            type="number" 
                            className="bg-zinc-900/50 border-zinc-800 focus:border-kiln-accent rounded-lg px-3 py-2 text-white font-mono"
                            value={tariff}
                            onChange={e => setTariff(Number(e.target.value))}
                        />
                    </div>
                    <div className="bg-kiln-card border border-kiln-border rounded-xl p-4 flex flex-col justify-between items-end">
                        <label className="text-[10px] font-bold text-zinc-500 uppercase tracking-widest">{t.schedules.estCost}</label>
                        <div className="text-2xl font-bold text-emerald-400 font-mono">
                             ${estimatedCost.toFixed(2)}
                        </div>
                    </div>
                </div>
            </>
        ) : (
            <div className="flex-1 flex flex-col items-center justify-center text-zinc-600">
                <div className="w-20 h-20 bg-kiln-card rounded-full flex items-center justify-center mb-6 shadow-lg shadow-black/20">
                    <Zap size={32} className="opacity-20" />
                </div>
                <p className="text-lg font-medium">{t.schedules.selectToEdit}</p>
                <p className="text-sm opacity-50">{t.schedules.orCreate}</p>
            </div>
        )}
      </div>
    </div>
  );
};

export default ScheduleEditor;
