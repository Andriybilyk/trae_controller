import React, { useEffect, useState } from 'react';
import { Line } from 'react-chartjs-2';
import { Chart as ChartJS, CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler } from 'chart.js';
import { Thermometer, Activity, Clock, Wind, DollarSign, Sliders, Play, Square, Timer } from 'lucide-react';
import { useLanguage } from '../contexts/LanguageContext';

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

const StatCard = ({ title, value, unit, colorClass, valueClass }: any) => (
  <div className="bg-kiln-card border border-kiln-border rounded-xl p-5 flex flex-col justify-center shadow-lg shadow-black/20 min-w-0 h-28">
    <div className="text-[10px] font-bold text-zinc-500 uppercase tracking-widest mb-2 truncate">{title}</div>
    <div className={`text-xl lg:text-2xl font-bold tracking-tight truncate ${valueClass || 'text-white'}`}>
      {value} <span className="text-sm text-zinc-600 font-normal ml-0.5 align-baseline">{unit}</span>
    </div>
  </div>
);

const Dashboard = () => {
  const { t } = useLanguage();
  const [status, setStatus] = useState<any>({ temp: 22.5, status: 'IDLE', energy: 0 });
  const [history, setHistory] = useState<{ time: string, temp: number, target: number }[]>([]);
  const [schedules, setSchedules] = useState<Schedule[]>([]);
  const [selectedScheduleId, setSelectedScheduleId] = useState<string>("");

  useEffect(() => {
    // Fetch Status from Backend
    const fetchStatus = async () => {
        try {
            const res = await fetch('http://localhost:3001/api/status');
            if (res.ok) {
                const data = await res.json();
                setStatus(data);
                
                // Update History
                setHistory(prev => {
                    const newTime = new Date().toLocaleTimeString('en-US', { hour12: false });
                    const newEntry = { time: newTime, temp: data.temp, target: data.target };
                    const newHistory = [...prev, newEntry];
                    if (newHistory.length > 60) newHistory.shift(); // Keep last 60 points
                    return newHistory;
                });
            }
        } catch (e) {
            console.error("Connection error");
        }
    };

    // Fetch Schedules
    const fetchSchedules = async () => {
        try {
            const res = await fetch('http://localhost:3000/api/schedules');
            if (res.ok) {
                const data = await res.json();
                setSchedules(data);
                if (data.length > 0 && !selectedScheduleId) {
                    setSelectedScheduleId(data[0].id);
                }
            }
        } catch (e) {
            console.error("Failed to fetch schedules");
        }
    };

    fetchSchedules();
    const interval = setInterval(fetchStatus, 1000);
    return () => clearInterval(interval);
  }, []);

  const handleStartFiring = async () => {
      const schedule = schedules.find(s => s.id === selectedScheduleId);
      if (!schedule) {
          alert("Please select a schedule first");
          return;
      }

      try {
          await fetch('http://localhost:3000/api/start', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify(schedule)
          });
      } catch (e) {
          alert("Error starting firing");
      }
  };

  const [showStopConfirm, setShowStopConfirm] = useState(false);

  // ... (existing useEffects)

  const handleStopFiring = async () => {
      // Instead of browser confirm, show custom UI
      setShowStopConfirm(true);
  };

  const confirmStop = async () => {
      try {
          await fetch('http://localhost:3000/api/stop', { method: 'POST' });
          setShowStopConfirm(false);
      } catch (e) {
          alert("Error stopping firing");
      }
  };

  const formatTimeRemaining = (status: any) => {
      if (status.status === 'IDLE' || status.status === 'COMPLETE' || status.status === 'ERROR') return "-- : --";
      if (!status.estimatedDuration) return "-- : --";
      
      const remainingMinutes = Math.max(0, status.estimatedDuration - status.elapsedTime);
      const hours = Math.floor(remainingMinutes / 60);
      const minutes = Math.floor(remainingMinutes % 60);
      
      return `${hours.toString().padStart(2, '0')} : ${minutes.toString().padStart(2, '0')}`;
  };

  return (
    <div className="flex flex-col gap-6 p-8 h-full overflow-y-auto bg-kiln-bg relative">
      
      {/* Custom Stop Confirmation Modal */}
      {showStopConfirm && (
          <div className="absolute inset-0 z-50 bg-black/80 backdrop-blur-sm flex items-center justify-center p-4">
              <div className="bg-kiln-card border border-red-500/50 rounded-2xl p-8 max-w-md w-full shadow-[0_0_50px_rgba(239,68,68,0.2)] text-center animate-in fade-in zoom-in duration-200">
                  <div className="w-20 h-20 bg-red-500/10 rounded-full flex items-center justify-center mx-auto mb-6">
                      <Square size={40} className="text-red-500" fill="currentColor" />
                  </div>
                  
                  <h3 className="text-2xl font-bold text-white mb-2">{t.dashboard.stopFiring}?</h3>
                  <p className="text-zinc-400 mb-8">
                      Are you sure you want to abort the current firing process? This action cannot be undone.
                  </p>
                  
                  <div className="flex flex-col gap-3">
                      <button 
                          onClick={confirmStop}
                          className="w-full py-4 bg-red-600 hover:bg-red-500 text-white rounded-xl font-bold text-lg transition-all shadow-lg shadow-red-900/40"
                      >
                          {t.dashboard.stopFiring}
                      </button>
                      <button 
                          onClick={() => setShowStopConfirm(false)}
                          className="w-full py-3 bg-zinc-800 hover:bg-zinc-700 text-zinc-300 rounded-xl font-medium transition-colors"
                      >
                          Cancel
                      </button>
                  </div>
              </div>
          </div>
      )}

      {/* Stat Cards Row */}
      <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-5 gap-4">
        <StatCard 
            title={t.dashboard.currentTemp} 
            value={status.temp.toFixed(1)} 
            unit="°C" 
        />
        <StatCard 
            title={t.dashboard.status} 
            value={t.status[status.status] || status.status} 
            unit="" 
            valueClass={status.status === 'IDLE' ? 'text-zinc-400' : 'text-kiln-accent'}
        />
        <StatCard 
            title={t.dashboard.timeRemaining} 
            value={formatTimeRemaining(status)} 
            unit="" 
        />
        <StatCard 
            title={t.dashboard.currentCost} 
            value="$0.00" 
            unit="" 
            valueClass="text-emerald-400"
        />
        <StatCard 
            title={t.dashboard.ventFan} 
            value={t.status.OFF} 
            unit="" 
            valueClass="text-zinc-500"
        />
      </div>

      {/* Main Chart Section */}
      <div className="flex-1 bg-kiln-card border border-kiln-border rounded-xl shadow-lg shadow-black/20 flex flex-col overflow-hidden min-h-[400px]">
        {/* Header */}
        <div className="p-5 border-b border-kiln-border flex flex-col md:flex-row justify-between items-center gap-4 bg-kiln-card/50">
            <h2 className="text-lg font-bold text-white flex items-center gap-2">
                {t.dashboard.firingProfile}
            </h2>
            
            <div className="flex items-center gap-3">
                 <select 
                    className="bg-zinc-900 border border-zinc-700 text-white text-sm rounded-lg focus:ring-kiln-accent focus:border-kiln-accent block p-2.5 min-w-[200px]"
                    value={selectedScheduleId}
                    onChange={(e) => setSelectedScheduleId(e.target.value)}
                 >
                     <option value="" disabled>{t.dashboard.selectSchedule}</option>
                     {schedules.map(s => (
                         <option key={s.id} value={s.id}>{s.name}</option>
                     ))}
                 </select>

                 <button className="flex items-center gap-2 px-4 py-2 bg-amber-600/10 text-amber-500 border border-amber-600/20 rounded-lg hover:bg-amber-600/20 transition-colors text-sm font-medium">
                    <Timer size={16} /> {t.dashboard.delayStart}
                 </button>
                 
                 {status.status === 'IDLE' || status.status === 'COMPLETE' || status.status === 'ERROR' ? (
                     <button 
                        onClick={handleStartFiring}
                        className="flex items-center gap-2 px-5 py-2 bg-kiln-accent text-black rounded-lg hover:bg-emerald-400 transition-colors text-sm font-bold shadow-[0_0_15px_rgba(16,185,129,0.2)]"
                     >
                        <Play size={16} fill="black" /> {t.dashboard.startFiring}
                     </button>
                 ) : (
                     <button 
                        onClick={handleStopFiring}
                        className="flex items-center gap-2 px-5 py-2 bg-red-500 text-white rounded-lg hover:bg-red-600 transition-colors text-sm font-bold shadow-[0_0_15px_rgba(239,68,68,0.2)]"
                     >
                        <Square size={16} fill="white" /> {t.dashboard.stopFiring}
                     </button>
                 )}
            </div>
        </div>

        {/* Chart */}
        <div className="flex-1 p-6 relative">
            <Line options={chartOptions} data={{
                labels: history.map(h => h.time),
                datasets: [
                    { 
                        label: 'Temperature',
                        data: history.map(h => h.temp), 
                        borderColor: '#3b82f6', // Blue trace like in screenshot? Or Green? Screenshot has flat line. Let's stick to blue/theme.
                        // Screenshot image_2 shows a flat blue line near 0.
                        backgroundColor: (context: any) => {
                            const ctx = context.chart.ctx;
                            const gradient = ctx.createLinearGradient(0, 0, 0, 400);
                            gradient.addColorStop(0, 'rgba(59, 130, 246, 0.1)');
                            gradient.addColorStop(1, 'rgba(59, 130, 246, 0)');
                            return gradient;
                        },
                        borderWidth: 2,
                        fill: true,
                        tension: 0.4,
                        pointRadius: 0
                    },
                    { 
                        label: 'Setpoint',
                        data: history.map(h => h.target), 
                        borderColor: '#f97316', // Orange setpoint
                        borderDash: [4, 4], 
                        borderWidth: 1,
                        pointRadius: 0
                    }
                ]
            }} />
            
            <div className="absolute bottom-6 left-16 text-xs text-orange-500 font-medium">
                {t.dashboard.setpoint}
            </div>
        </div>
      </div>
    </div>
  );
};

export default Dashboard;
