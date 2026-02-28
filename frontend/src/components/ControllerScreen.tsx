import React, { useState, useEffect } from 'react';
import { useLanguage } from '../contexts/LanguageContext';
import { Play, Square, Thermometer, Clock, Activity, Wifi, Settings, AlertTriangle, ArrowRight, X } from 'lucide-react';
import { API_BASE_URL } from '../config';

// Types
interface StatusData {
    temp: number;
    target: number;
    status: string;
    step: number;
    totalSteps: number;
    timeRemaining?: number;
    error?: string;
}

interface Schedule {
    id: string;
    name: string;
    steps: any[];
}

// --- SUB-COMPONENTS ---

const formatMinutesToHM = (minutes: number | undefined) => {
    if (minutes === undefined || minutes === null || isNaN(minutes) || minutes < 0) return '--:--';
    const hours = Math.floor(minutes / 60);
    const mins = Math.floor(minutes % 60);
    return `${hours.toString().padStart(2, '0')}:${mins.toString().padStart(2, '0')}`;
};

const DashboardView = ({ 
    status, 
    selectedSchedule, 
    t, 
    onStart, 
    onStop, 
    onOpenLibrary 
}: { 
    status: StatusData; 
    selectedSchedule: Schedule | null; 
    t: any; 
    onStart: () => void; 
    onStop: () => void; 
    onOpenLibrary: () => void; 
}) => (
    <div className="grid grid-cols-2 h-full">
        {/* Left Column: Temperature & Status */}
        <div className="p-6 border-r border-zinc-800 flex flex-col justify-between relative">
            
            {/* Status Badge */}
            <div className="absolute top-6 right-6">
                 <span className={`px-4 py-2 rounded-full text-white font-bold text-sm ${status.status === 'IDLE' ? 'bg-zinc-800' : status.status === 'ERROR' ? 'bg-red-500' : 'bg-kiln-accent text-black'}`}>
                    {status.status}
                </span>
            </div>

            <div>
                <div className="text-zinc-500 font-bold uppercase tracking-widest mb-2">{t.dashboard.currentTemp}</div>
                <div className="text-8xl font-bold text-white font-mono flex items-start leading-none tracking-tighter">
                    {status.temp.toFixed(1)}
                    <span className="text-4xl text-zinc-600 mt-4 ml-2">°C</span>
                </div>
            </div>

            <div className="bg-zinc-900/50 rounded-2xl p-6 border border-zinc-800">
                <div className="mb-4 space-y-4">
                    <div className="flex justify-between items-end">
                        <span className="text-zinc-400 font-medium">{t.schedules.targetTemp}</span>
                        <span className="text-4xl text-white font-mono leading-none">{status.target}°C</span>
                    </div>
                    
                    { (status.status !== 'IDLE' && status.status !== 'COMPLETE' && status.status !== 'ERROR' && status.timeRemaining !== undefined) && (
                        <div className="flex justify-between items-end">
                            <span className="text-zinc-400 font-medium">{t.dashboard.timeRemaining}</span>
                            <span className="text-4xl text-white font-mono leading-none">{formatMinutesToHM(status.timeRemaining)}</span>
                        </div>
                    )}
                </div>
                
                {/* Progress Bar */}
                <div className="w-full bg-zinc-800 rounded-full h-3 overflow-hidden">
                    <div 
                        className="bg-kiln-accent h-full transition-all duration-500" 
                        style={{ width: status.target > 0 ? `${Math.min((status.temp / status.target) * 100, 100)}%` : '0%' }}
                    ></div>
                </div>
            </div>
        </div>

        {/* Right Column: Controls */}
        <div className="p-6 flex flex-col gap-4">
            {/* Active Program Info */}
            <div className="flex-1 bg-zinc-900/30 rounded-2xl p-6 border border-zinc-800 flex flex-col justify-center items-center text-center relative overflow-hidden group">
                {status.status === 'IDLE' ? (
                    selectedSchedule ? (
                        <div onClick={onOpenLibrary} className="cursor-pointer w-full h-full flex flex-col items-center justify-center">
                            <div className="text-kiln-accent font-bold mb-2 uppercase tracking-wider text-xs">{t.schedules.selectedProgram}</div>
                            <div className="text-3xl font-bold text-white mb-4">{selectedSchedule.name}</div>
                            <div className="text-zinc-500 text-sm">{selectedSchedule.steps.length} {t.schedules.segments} • {t.schedules.selectToEdit}</div>
                        </div>
                    ) : (
                        <div onClick={onOpenLibrary} className="cursor-pointer w-full h-full flex flex-col items-center justify-center hover:bg-zinc-800/50 transition-colors rounded-xl">
                            <div className="mb-4 opacity-30"><Activity size={64} className="mx-auto" /></div>
                            <div className="text-xl font-bold text-zinc-400">{t.dashboard.selectSchedule}</div>
                            <div className="text-sm text-zinc-600 mt-2">{t.schedules.orCreate}</div>
                        </div>
                    )
                ) : (
                    <>
                         <div className="text-zinc-500 uppercase text-xs font-bold tracking-widest mb-2">{t.dashboard.stepHold}</div>
                         <div className="text-6xl font-bold text-white mb-2 font-mono">{status.step} <span className="text-2xl text-zinc-600">/ {status.totalSteps}</span></div>
                         <div className="text-zinc-400">Ramp to {status.target}°C</div>
                    </>
                )}
            </div>

            {/* Big Action Buttons */}
            <div className="grid grid-cols-2 gap-4 h-32">
                {status.status === 'IDLE' || status.status === 'COMPLETE' || status.status === 'ERROR' ? (
                    <button 
                        onClick={onStart}
                        className={`rounded-2xl font-bold text-xl flex flex-col items-center justify-center gap-2 transition-all active:scale-95 ${selectedSchedule ? 'bg-kiln-accent hover:bg-emerald-400 text-black shadow-[0_0_30px_rgba(16,185,129,0.2)]' : 'bg-zinc-800 text-zinc-500'}`}
                    >
                        <Play size={32} fill={selectedSchedule ? "black" : "currentColor"} />
                        {t.dashboard.startFiring}
                    </button>
                ) : (
                    <button 
                        onClick={onStop}
                        className="bg-red-500 hover:bg-red-400 text-white rounded-2xl font-bold text-xl flex flex-col items-center justify-center gap-2 transition-all active:scale-95 shadow-[0_0_30px_rgba(239,68,68,0.3)] col-span-2"
                    >
                        <Square size={32} fill="white" />
                        {t.dashboard.stopFiring}
                    </button>
                )}

                {(status.status === 'IDLE' || status.status === 'COMPLETE' || status.status === 'ERROR') && (
                    <button onClick={onOpenLibrary} className="bg-zinc-800 hover:bg-zinc-700 text-white rounded-2xl font-bold text-xl flex flex-col items-center justify-center gap-2 transition-colors active:scale-95 border border-zinc-700">
                        <Thermometer size={32} />
                        {t.nav.schedules}
                    </button>
                )}
            </div>
        </div>
    </div>
);

const EditorView = ({
    editingSchedule,
    setEditingSchedule,
    onSave,
    onClose,
    onEditStep,
    onAddStep,
    onRemoveStep,
    t
}: {
    editingSchedule: Schedule;
    setEditingSchedule: (s: Schedule) => void;
    onSave: () => void;
    onClose: () => void;
    onEditStep: (idx: number, field: string, val: any) => void;
    onAddStep: () => void;
    onRemoveStep: (idx: number) => void;
    t: any;
}) => {
    if (!editingSchedule) return null;
    
    return (
        <div className="h-full flex flex-col p-6">
            {/* Header */}
            <div className="flex justify-between items-center mb-4 shrink-0">
                <div className="flex-1">
                    <input 
                        className="text-2xl font-bold text-white bg-transparent border-none p-0 w-full focus:ring-0 focus:border-b focus:border-kiln-accent placeholder-zinc-600"
                        value={editingSchedule.name}
                        onChange={(e) => setEditingSchedule({ ...editingSchedule, name: e.target.value })}
                        placeholder={t.schedules.programName}
                    />
                </div>
                <div className="flex gap-2">
                    <button onClick={onSave} className="flex items-center gap-2 px-6 py-2 bg-kiln-accent hover:bg-emerald-400 text-black rounded-xl font-bold transition-colors">
                        <Settings size={18} /> Save
                    </button>
                    <button onClick={onClose} className="p-3 bg-zinc-800 hover:bg-zinc-700 rounded-xl text-white">
                        <X size={24} />
                    </button>
                </div>
            </div>

            {/* Steps List */}
            <div className="flex-1 overflow-y-auto pr-2 custom-scrollbar space-y-3">
                {editingSchedule.steps.length === 0 && (
                    <div className="text-center py-12 text-zinc-500 border-2 border-dashed border-zinc-800 rounded-2xl">
                        {t.schedules.noSteps}
                    </div>
                )}
                
                {editingSchedule.steps.map((step: any, index: number) => (
                    <div key={index} className="bg-zinc-900 rounded-xl p-4 border border-zinc-800 flex items-center gap-4">
                        <div className="w-8 h-8 rounded-full bg-zinc-800 flex items-center justify-center font-bold text-zinc-500 text-sm">
                            {index + 1}
                        </div>
                        
                        <div className="flex-1 grid grid-cols-3 gap-4">
                            <div className="flex flex-col">
                                <label className="text-[12px] text-zinc-500 uppercase font-bold">{t.schedules.rateLabel}</label>
                                <input 
                                    type="number" 
                                    className="bg-transparent text-white font-mono font-bold text-2xl w-full border-b border-zinc-700 focus:border-kiln-accent focus:outline-none"
                                    value={step.rate}
                                    onChange={(e) => onEditStep(index, 'rate', e.target.value)}
                                />
                            </div>
                            <div className="flex flex-col">
                                <label className="text-[12px] text-zinc-500 uppercase font-bold">{t.schedules.targetLabel}</label>
                                <input 
                                    type="number" 
                                    className="bg-transparent text-white font-mono font-bold text-2xl w-full border-b border-zinc-700 focus:border-kiln-accent focus:outline-none"
                                    value={step.target}
                                    onChange={(e) => onEditStep(index, 'target', e.target.value)}
                                />
                            </div>
                            <div className="flex flex-col">
                                <label className="text-[12px] text-zinc-500 uppercase font-bold">{t.schedules.holdLabel}</label>
                                <input 
                                    type="number" 
                                    className="bg-transparent text-white font-mono font-bold text-2xl w-full border-b border-zinc-700 focus:border-kiln-accent focus:outline-none"
                                    value={step.holdTime}
                                    onChange={(e) => onEditStep(index, 'holdTime', e.target.value)}
                                />
                            </div>
                        </div>

                        <button onClick={() => onRemoveStep(index)} className="p-2 text-zinc-600 hover:text-red-500 transition-colors">
                            <X size={20} />
                        </button>
                    </div>
                ))}

                <button 
                    onClick={onAddStep}
                    className="w-full py-4 border-2 border-dashed border-zinc-800 rounded-xl text-zinc-500 hover:text-white hover:border-zinc-600 transition-colors font-bold flex items-center justify-center gap-2"
                >
                    {t.schedules.addStep}
                </button>
            </div>
        </div>
    );
};

const SchedulesView = React.memo(({
    schedules,
    selectedSchedule,
    t,
    onSelect,
    onEdit,
    onNew,
    onClose
}: {
    schedules: Schedule[];
    selectedSchedule: Schedule | null;
    t: any;
    onSelect: (s: Schedule) => void;
    onEdit: (s: Schedule) => void;
    onNew: () => void;
    onClose: () => void;
}) => (
    <div className="h-full flex flex-col p-6">
        <div className="flex justify-between items-center mb-6 shrink-0">
            <h2 className="text-2xl font-bold text-white flex items-center gap-3">
                <Thermometer className="text-kiln-accent" /> {t.schedules.library}
            </h2>
            <button onClick={onClose} className="p-3 bg-zinc-800 rounded-xl text-white">
                <X size={24} />
            </button>
        </div>
        
        <div className="grid grid-cols-2 gap-4 overflow-y-auto pb-4 pr-2 custom-scrollbar">
            {schedules.map(s => (
                <button 
                    key={s.id}
                    onClick={() => onEdit(s)}
                    className={`p-6 rounded-2xl text-left border transition-all active:scale-98 group flex items-center justify-between ${selectedSchedule?.id === s.id ? 'bg-zinc-800 border-kiln-accent ring-2 ring-kiln-accent/50' : 'bg-zinc-900 border-zinc-800 hover:bg-zinc-800'}`}
                >
                    <div className="flex-1 overflow-hidden">
                        <div className="text-xl font-bold text-white mb-2 truncate">{s.name}</div>
                        <div className="flex justify-between text-zinc-500 text-sm mb-4">
                            <span>{s.steps.length} {t.schedules.segments}</span>
                            <span>Custom</span>
                        </div>
                    </div>
                    
                    {/* Quick Select Button */}
                    <div 
                        className="ml-4 shrink-0"
                        onClick={(e) => {
                            e.stopPropagation();
                            onSelect(s);
                        }}
                    >
                         <div className={`w-16 h-16 rounded-full flex items-center justify-center transition-colors ${selectedSchedule?.id === s.id ? 'bg-kiln-accent text-black' : 'bg-zinc-700 text-white group-hover:bg-kiln-accent group-hover:text-black'}`}>
                            <Play size={24} fill="currentColor" />
                         </div>
                    </div>
                </button>
            ))}
            
            {/* Add New Button */}
             <button 
                onClick={onNew}
                className="p-6 rounded-2xl border border-dashed border-zinc-700 flex flex-col items-center justify-center gap-3 text-zinc-500 hover:text-white hover:border-zinc-500 transition-colors"
            >
                <div className="w-12 h-12 rounded-full bg-zinc-800 flex items-center justify-center">
                    <Settings size={24} />
                </div>
                <span className="font-medium">{t.schedules.newSchedule}</span>
            </button>
        </div>
    </div>
));

const ControllerScreen = () => {
    const { t, setLanguage, language } = useLanguage();
    const [status, setStatus] = useState<StatusData>({
        temp: 24.5,
        target: 0,
        status: 'IDLE',
        step: 0,
        totalSteps: 0
    });
    
    // UI State
    const [view, setView] = useState<'DASHBOARD' | 'SCHEDULES' | 'EDITOR'>('DASHBOARD');
    const [schedules, setSchedules] = useState<Schedule[]>([]);
    const [selectedSchedule, setSelectedSchedule] = useState<Schedule | null>(null);
    const [editingSchedule, setEditingSchedule] = useState<Schedule | null>(null);
    const [currentTime, setCurrentTime] = useState(new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }));
    
    const [showStopConfirm, setShowStopConfirm] = useState(false);

    // Fetch Status Loop
    useEffect(() => {
        const interval = setInterval(async () => {
            try {
                // Update Time
                setCurrentTime(new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }));

                // Fetch Status
                const res = await fetch(`${API_BASE_URL}/status`);
                if (res.ok) {
                    const data = await res.json();
                    setStatus(data);
                }
            } catch (e) {
                // console.error("Sim connection error");
            }
        }, 1000);
        return () => clearInterval(interval);
    }, []);

    // Fetch Schedules when entering schedule view
    useEffect(() => {
        if (view === 'SCHEDULES') {
            fetch(`${API_BASE_URL}/schedules`)
                .then(res => res.json())
                .then(data => setSchedules(data))
                .catch(e => console.error(e));
        }
    }, [view]);

    const handleStartFiring = async () => {
        if (!selectedSchedule) {
            setView('SCHEDULES');
            return;
        }
        try {
            await fetch(`${API_BASE_URL}/start`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(selectedSchedule)
            });
            setView('DASHBOARD');
        } catch (e) {
            alert("Error starting");
        }
    };

    const handleSaveSchedule = async () => {
        if (!editingSchedule) return;
        try {
            await fetch(`${API_BASE_URL}/schedules`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(editingSchedule)
            });
            // Reload list
            const res = await fetch(`${API_BASE_URL}/schedules`);
            const data = await res.json();
            setSchedules(data);
            
            // If this was a new schedule, select it
            if (!selectedSchedule || selectedSchedule.id === editingSchedule.id) {
                setSelectedSchedule(editingSchedule);
            }
            setView('SCHEDULES');
        } catch (e) {
            console.error("Failed to save");
        }
    };

    const handleEditStep = (stepIndex: number, field: string, value: any) => {
        if (!editingSchedule) return;
        const newSteps = [...editingSchedule.steps];
        newSteps[stepIndex] = { ...newSteps[stepIndex], [field]: Number(value) };
        setEditingSchedule({ ...editingSchedule, steps: newSteps });
    };

    const handleAddStep = () => {
        if (!editingSchedule) return;
        const lastStep = editingSchedule.steps[editingSchedule.steps.length - 1];
        const newStep = {
            id: Date.now(),
            type: 'ramp',
            rate: 100,
            target: lastStep ? lastStep.target : 100,
            holdTime: 0
        };
        setEditingSchedule({ ...editingSchedule, steps: [...editingSchedule.steps, newStep] });
    };

    const handleRemoveStep = (index: number) => {
        if (!editingSchedule) return;
        const newSteps = editingSchedule.steps.filter((_, i) => i !== index);
        setEditingSchedule({ ...editingSchedule, steps: newSteps });
    };

    const handleNewSchedule = () => {
        setEditingSchedule({
            id: crypto.randomUUID(),
            name: "New Program",
            steps: []
        });
        setView('EDITOR');
    };

    const handleStopFiring = () => {
        setShowStopConfirm(true);
    };

    const confirmStop = async () => {
        await fetch(`${API_BASE_URL}/stop`, { method: 'POST' });
        setShowStopConfirm(false);
    };
    
    // Handlers for SchedulesView
    const handleSelectSchedule = (s: Schedule) => {
        setSelectedSchedule(s);
        setView('DASHBOARD');
    };

    const handleEditSchedule = (s: Schedule) => {
        setEditingSchedule(s);
        setView('EDITOR');
    };

    return (
        <div className="flex flex-col items-center justify-center min-h-screen bg-zinc-950 p-8 select-none">
            <h2 className="text-zinc-500 mb-4 font-mono text-sm">Waveshare 4.3" ESP32-S3 Simulator (800x480)</h2>
            
            {/* The Screen Container */}
            <div 
                className="relative bg-black overflow-hidden shadow-2xl border-[12px] border-zinc-900 rounded-[2rem]"
                style={{ width: '800px', height: '480px' }}
            >
                {/* Header Bar */}
                <div className="h-14 bg-zinc-900/80 backdrop-blur-md border-b border-zinc-800 flex justify-between items-center px-6 absolute top-0 left-0 right-0 z-10">
                    <div className="flex items-center gap-3">
                        <Activity className="text-kiln-accent" size={20} />
                        <span className="text-lg font-bold text-white tracking-wider">KILN PRO</span>
                    </div>
                    <div className="flex items-center gap-4 text-zinc-400 font-mono text-sm">
                        <button 
                            onClick={() => setLanguage(language === 'en' ? 'ua' : 'en')} 
                            className="px-3 py-1 bg-zinc-800 rounded-lg hover:bg-zinc-700 text-white font-bold uppercase transition-colors text-xs tracking-wider"
                        >
                            {language}
                        </button>
                        <span>{currentTime}</span>
                        <Wifi size={18} className={status.status !== 'ERROR' ? "text-emerald-500" : "text-zinc-600"} />
                    </div>
                </div>

                {/* Main Content Area (With Padding for Header) */}
                <div className="pt-14 h-full bg-black text-white relative">
                    {view === 'DASHBOARD' && (
                        <DashboardView 
                            status={status}
                            selectedSchedule={selectedSchedule}
                            t={t}
                            onStart={handleStartFiring}
                            onStop={handleStopFiring}
                            onOpenLibrary={() => setView('SCHEDULES')}
                        />
                    )}
                    
                    {view === 'SCHEDULES' && (
                        <SchedulesView 
                            schedules={schedules}
                            selectedSchedule={selectedSchedule}
                            t={t}
                            onSelect={handleSelectSchedule}
                            onEdit={handleEditSchedule}
                            onNew={handleNewSchedule}
                            onClose={() => setView('DASHBOARD')}
                        />
                    )}
                    
                    {view === 'EDITOR' && editingSchedule && (
                        <EditorView 
                            editingSchedule={editingSchedule}
                            setEditingSchedule={setEditingSchedule}
                            onSave={handleSaveSchedule}
                            onClose={() => setView('SCHEDULES')}
                            onEditStep={handleEditStep}
                            onAddStep={handleAddStep}
                            onRemoveStep={handleRemoveStep}
                            t={t}
                        />
                    )}

                    {/* Stop Confirmation Modal */}
                    {showStopConfirm && (
                        <div className="absolute inset-0 z-50 bg-black/90 backdrop-blur-sm flex items-center justify-center p-8">
                            <div className="bg-zinc-900 border border-zinc-700 rounded-2xl p-8 w-full max-w-md text-center shadow-2xl">
                                <div className="w-20 h-20 bg-red-500/10 rounded-full flex items-center justify-center mx-auto mb-6">
                                    <Square size={40} className="text-red-500" fill="currentColor" />
                                </div>
                                <h3 className="text-3xl font-bold text-white mb-2">{t.dashboard.stopFiring}?</h3>
                                <p className="text-zinc-400 mb-8 text-lg">Are you sure you want to abort the process?</p>
                                
                                <div className="grid grid-cols-2 gap-4">
                                    <button 
                                        onClick={() => setShowStopConfirm(false)}
                                        className="py-4 bg-zinc-800 hover:bg-zinc-700 text-white rounded-xl font-bold text-xl"
                                    >
                                        Cancel
                                    </button>
                                    <button 
                                        onClick={confirmStop}
                                        className="py-4 bg-red-600 hover:bg-red-500 text-white rounded-xl font-bold text-xl shadow-lg shadow-red-900/40"
                                    >
                                        STOP
                                    </button>
                                </div>
                            </div>
                        </div>
                    )}
                </div>
            </div>
            
            <p className="mt-8 text-zinc-600 max-w-lg text-center text-xs leading-relaxed">
                Use this preview to verify the layout for the embedded display. 
                Touch targets are sized for fingers (min 44px). 
                Fonts are optimized for legibility at arm's length.
            </p>
        </div>
    );
};

export default ControllerScreen;
