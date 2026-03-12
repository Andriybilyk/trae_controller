
import React, { useState, useEffect } from 'react';
import { API_BASE_URL } from '../config';
import { useLanguage } from '../contexts/LanguageContext';
import { Clock, Thermometer, Hash, AlertTriangle, CheckCircle, XCircle, Loader, X } from 'lucide-react';
import { Line } from 'react-chartjs-2';
import { Chart as ChartJS, CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler } from 'chart.js';

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler);

// --- Type Definitions ---
interface FiringSummary {
    id: string;
    scheduleName: string;
    startTime: number;
    endTime: number;
    duration: number; // in seconds
    status: 'COMPLETED' | 'ERROR' | 'STOPPED';
    peakTemp: number | null;
    totalSteps: number;
    completedSteps: number;
}

interface FiringDataPoint {
    timestamp: number;
    temp: number;
    target: number;
}

interface FiringDetail {
    summary: FiringSummary;
    data: FiringDataPoint[];
}

// --- Chart Options ---
const chartOptions = {
  responsive: true,
  maintainAspectRatio: false,
  scales: {
    x: {
        type: 'linear' as const,
        ticks: {
            color: '#71717a',
            callback: (value: any, index: number, values: any) => {
                // Custom callback to format timestamp to relative time (e.g., 5m, 1h 15m)
                const seconds = value / 1000;
                const h = Math.floor(seconds / 3600);
                const m = Math.floor((seconds % 3600) / 60);
                if (h > 0) return `${h}h ${m}m`;
                return `${m}m`;
            }
        },
        grid: { color: '#27272a' },
        title: { display: true, text: 'Time From Start', color: '#a1a1aa' }
    },
    y: { 
        min: 0, 
        grid: { color: '#27272a' }, 
        ticks: { color: '#71717a' },
        title: { display: true, text: 'Temperature (°C)', color: '#a1a1aa' }
    }
  },
  plugins: {
    legend: { position: 'top' as const, labels: { color: '#a1a1aa' } },
    tooltip: { backgroundColor: '#18181b', titleColor: '#fff', bodyColor: '#a1a1aa' }
  }
};

// --- Main Component ---
const History = () => {
    const { t } = useLanguage();
    const [history, setHistory] = useState<FiringSummary[]>([]);
    const [loading, setLoading] = useState(true);
    const [error, setError] = useState<string | null>(null);
    
    const [selectedFiring, setSelectedFiring] = useState<FiringDetail | null>(null);
    const [isDetailLoading, setIsDetailLoading] = useState(false);

    const fetchHistory = async () => {
        setLoading(true);
        try {
            const response = await fetch(`${API_BASE_URL}/history`);
            if (!response.ok) throw new Error('Failed to fetch history data.');
            const data = await response.json();
            setHistory(data);
        } catch (err) {
            setError(err.message);
        } finally {
            setLoading(false);
        }
    };

    useEffect(() => {
        fetchHistory();
    }, []);

    const handleSelectFiring = async (id: string) => {
        setIsDetailLoading(true);
        try {
            const response = await fetch(`${API_BASE_URL}/history/${id}`);
            if (!response.ok) throw new Error('Failed to load firing details.');
            const data: FiringDetail = await response.json();
            
            // Validate data
            if (!data || !data.summary) {
                console.warn("Invalid data received", data);
                // Fallback: try to find summary in main list if detail fetch failed but returned 200 (edge case)
                const summary = history.find(h => h.id === id);
                if (summary) {
                    setSelectedFiring({ summary, data: [] });
                } else {
                    throw new Error("Data invalid");
                }
            } else {
                setSelectedFiring(data);
            }
        } catch (err) {
            console.error(err);
            // Don't show full error UI, just log and maybe show toast
            // Or set a partial state if possible
        } finally {
            setIsDetailLoading(false);
        }
    };

    const formatDuration = (seconds: number) => {
        const h = Math.floor(seconds / 3600);
        const m = Math.floor((seconds % 3600) / 60);
        return `${h > 0 ? `${h}h ` : ''}${m}m`;
    };

    const StatusBadge = ({ status }: { status: FiringSummary['status'] }) => {
        const statusMap = {
            COMPLETED: { icon: <CheckCircle size={14} />, text: t.history.completed, className: 'bg-green-500/10 text-green-400 border-green-500/20' },
            ERROR: { icon: <AlertTriangle size={14} />, text: t.history.error, className: 'bg-red-500/10 text-red-400 border-red-500/20' },
            STOPPED: { icon: <XCircle size={14} />, text: t.history.stopped, className: 'bg-yellow-500/10 text-yellow-400 border-yellow-500/20' },
        };
        const current = statusMap[status] || statusMap.STOPPED;
        return (
            <div className={`flex items-center gap-1.5 px-2.5 py-1 rounded-full text-xs font-medium border ${current.className}`}>
                {current.icon} <span>{current.text}</span>
            </div>
        );
    };

    // --- Render Logic ---
    if (loading) {
        return <div className="flex justify-center items-center h-64"><Loader className="animate-spin text-kiln-accent" size={48} /></div>;
    }

    if (error) {
        return <div className="text-center text-red-400 bg-red-500/10 p-8 rounded-xl"><h2 className="font-bold text-lg">{t.history.error}</h2><p>{error}</p></div>;
    }

    return (
        <div className="max-w-7xl mx-auto">
            <h1 className="text-3xl font-bold text-white mb-6">{t.history.title}</h1>

            {history.length === 0 ? (
                <div className="text-center text-zinc-500 py-20">
                    <Hash size={64} className="mx-auto opacity-10 mb-4" />
                    <h2 className="text-xl font-bold">{t.history.noHistory}</h2>
                    <p>{t.history.emptyState}</p>
                </div>
            ) : (
                <div className="bg-kiln-card border border-kiln-border rounded-xl shadow-lg overflow-hidden">
                    <ul className="divide-y divide-kiln-border">
                        {history.map((item) => (
                            <li key={item.id} onClick={() => handleSelectFiring(item.id)} className="p-4 md:p-6 hover:bg-zinc-800/50 transition-colors cursor-pointer">
                                <div className="grid grid-cols-2 md:grid-cols-5 gap-4 items-center">
                                    <div className="col-span-2 md:col-span-2">
                                        <p className="font-bold text-white truncate">{item.scheduleName}</p>
                                        <p className="text-xs text-zinc-400">{new Date(item.startTime).toLocaleString()}</p>
                                    </div>
                                    <div className="flex items-center gap-2 text-sm text-zinc-300"><Clock size={16} className="text-zinc-500" /><span>{formatDuration(item.duration)}</span></div>
                                    <div className="flex items-center gap-2 text-sm text-zinc-300"><Thermometer size={16} className="text-zinc-500" /><span>{typeof item.peakTemp === 'number' ? `${item.peakTemp.toFixed(0)}°C ${t.history.peakTemp}` : '--'}</span></div>
                                    <div className="flex justify-end">
                                        <StatusBadge status={item.status} />
                                    </div>
                                </div>
                                <div className="mt-2 flex items-center gap-2 text-xs text-zinc-500">
                                    <Hash size={14} />
                                    <span>{t.history.steps}: {item.completedSteps} / {item.totalSteps}</span>
                                </div>
                            </li>
                        ))}
                    </ul>
                </div>
            )}

            {/* Detail Modal */}
            {(selectedFiring || isDetailLoading) && (
                <div className="fixed inset-0 z-[100] bg-black/80 backdrop-blur-sm flex items-center justify-center p-4 animate-in fade-in duration-200">
                    <div className="bg-kiln-card border border-zinc-700 rounded-2xl shadow-2xl w-full max-w-4xl max-h-[90vh] flex flex-col">
                        {isDetailLoading ? (
                            <div className="flex justify-center items-center h-96"><Loader className="animate-spin text-kiln-accent" size={48} /></div>
                        ) : selectedFiring && selectedFiring.summary ? (
                            <>
                                <div className="flex justify-between items-center p-4 border-b border-kiln-border shrink-0">
                                    <div>
                                        <h2 className="text-xl font-bold text-white">{selectedFiring.summary.scheduleName}</h2>
                                        <p className="text-sm text-zinc-400">{new Date(selectedFiring.summary.startTime).toLocaleString()}</p>
                                        <p className="text-xs text-zinc-500 mt-1">{t.history.steps}: {selectedFiring.summary.completedSteps} / {selectedFiring.summary.totalSteps}</p>
                                    </div>
                                    <button onClick={() => setSelectedFiring(null)} className="p-2 rounded-full text-zinc-400 hover:bg-zinc-800 hover:text-white"><X size={20} /></button>
                                </div>
                                <div className="p-4 md:p-6 flex-1 overflow-hidden">
                                    {selectedFiring.data && selectedFiring.data.length > 1 ? (
                                        <Line options={chartOptions} data={{
                                            datasets: [
                                                {
                                                    label: t.history.targetTemp,
                                                    data: selectedFiring.data.map(p => ({ x: p.timestamp - selectedFiring.summary.startTime, y: p.target })),
                                                    borderColor: '#6366f1',
                                                    borderDash: [5, 5],
                                                    pointRadius: 0,
                                                    tension: 0.1
                                                },
                                                {
                                                    label: t.history.currentTemp,
                                                    data: selectedFiring.data.map(p => ({ x: p.timestamp - selectedFiring.summary.startTime, y: p.temp })),
                                                    borderColor: '#10b981',
                                                    backgroundColor: 'rgba(16, 185, 129, 0.1)',
                                                    fill: true,
                                                    pointRadius: 0,
                                                    tension: 0.4
                                                }
                                            ]
                                        }} />
                                    ) : (
                                        <div className="flex items-center justify-center h-full text-zinc-500 text-center">
                                            <p>{t.history.noData}</p>
                                        </div>
                                    )}
                                </div>
                            </>
                        ) : (
                             <div className="p-8 text-center text-red-400">
                                 <p>{t.history.errorDetails}</p>
                                 <button onClick={() => setSelectedFiring(null)} className="mt-4 px-4 py-2 bg-zinc-800 rounded-md text-white">{t.dashboard.done}</button>
                             </div>
                        )}
                    </div>
                </div>
            )}
        </div>
    );
};

export default History;
