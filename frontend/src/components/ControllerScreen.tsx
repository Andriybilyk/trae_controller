import React, { useState, useEffect } from 'react';
import { useLanguage } from '../contexts/LanguageContext';
import { Play, Square, Thermometer, Clock, Activity, Wifi, Settings, AlertTriangle, ArrowRight, X, Trash2 } from 'lucide-react';
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
    stepsCount?: number;
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
}) => {
    // Unified Idle Logic
    const isIdle = status.status === 'IDLE' || status.status === 'COMPLETE' || status.status === 'ERROR';

    return (
        <div className="grid grid-cols-2 h-full gap-2">
            {/* Left Column: Temperature & Status */}
            <div className="p-3 border-r border-zinc-800 flex flex-col justify-between relative">
                
                {/* Status Badge */}
                <div className="absolute top-2 right-2">
                     <span className={`px-2 py-1 rounded-full text-white font-bold text-[10px] ${status.status === 'IDLE' ? 'bg-zinc-800' : status.status === 'ERROR' ? 'bg-red-500' : 'bg-kiln-accent text-black'}`}>
                        {status.status}
                    </span>
                </div>

                <div className="mt-4">
                    <div className="text-zinc-500 font-bold uppercase tracking-widest mb-0 text-[10px]">{t.dashboard.currentTemp}</div>
                    <div className="text-5xl font-bold text-white font-mono flex items-start leading-none tracking-tighter">
                        {status.temp.toFixed(1)}
                        <span className="text-xl text-zinc-600 mt-1 ml-1">??C</span>
                    </div>
                </div>

                <div className="bg-zinc-900/50 rounded-xl p-3 border border-zinc-800">
                    <div className="mb-2 space-y-1">
                        <div className="flex justify-between items-end">
                            <span className="text-zinc-400 font-medium text-[10px]">{t.schedules.targetTemp}</span>
                            <span className="text-xl text-white font-mono leading-none">{status.target}??C</span>
                        </div>
                        
                        { (!isIdle && status.timeRemaining !== undefined) && (
                            <div className="flex justify-between items-end">
                                <span className="text-zinc-400 font-medium text-[10px]">{t.dashboard.timeRemaining}</span>
                                <span className="text-xl text-white font-mono leading-none">{formatMinutesToHM(status.timeRemaining)}</span>
                            </div>
                        )}
                    </div>
                    
                    {/* Progress Bar */}
                    <div className="w-full bg-zinc-800 rounded-full h-2 overflow-hidden">
                        <div 
                            className="bg-kiln-accent h-full transition-all duration-500" 
                            style={{ width: status.target > 0 ? `${Math.min((status.temp / status.target) * 100, 100)}%` : '0%' }}
                        ></div>
                    </div>
                </div>
            </div>

            {/* Right Column: Controls */}
            <div className="p-3 flex flex-col gap-2">
                {/* Active Program Info */}
                <div className="flex-1 bg-zinc-900/30 rounded-xl p-2 border border-zinc-800 flex flex-col justify-center items-center text-center relative overflow-hidden group">
                    {isIdle ? (
                        selectedSchedule ? (
                            <div onClick={onOpenLibrary} className="cursor-pointer w-full h-full flex flex-col items-center justify-center">
                                <div className="text-kiln-accent font-bold mb-1 uppercase tracking-wider text-[10px]">{t.schedules.selectedProgram}</div>
                                <div className="text-lg font-bold text-white mb-1 leading-tight truncate w-full px-2">{selectedSchedule.name}</div>
                                <div className="text-zinc-500 text-[10px]">{selectedSchedule.steps ? selectedSchedule.steps.length : 0} {t.schedules.segments}</div>
                            </div>
                        ) : (
                            <div onClick={onOpenLibrary} className="cursor-pointer w-full h-full flex flex-col items-center justify-center hover:bg-zinc-800/50 transition-colors rounded-lg">
                                <div className="mb-1 opacity-30"><Activity size={32} className="mx-auto" /></div>
                                <div className="text-sm font-bold text-zinc-400">{t.dashboard.selectSchedule}</div>
                            </div>
                        )
                    ) : (
                        <>
                             <div className="text-zinc-500 uppercase text-[10px] font-bold tracking-widest mb-1">{t.dashboard.stepHold}</div>
                             <div className="text-4xl font-bold text-white mb-1 font-mono">{status.step} <span className="text-lg text-zinc-600">/ {status.totalSteps}</span></div>
                             <div className="text-zinc-400 text-[10px]">Ramp to {status.target}??C</div>
                        </>
                    )}
                </div>

                {/* Big Action Buttons */}
                <div className="grid grid-cols-2 gap-2 h-20">
                    {isIdle ? (
                        <button 
                            onClick={onStart}
                            disabled={!selectedSchedule}
                            className={`rounded-xl font-bold text-sm flex flex-col items-center justify-center gap-1 transition-all active:scale-95 ${selectedSchedule ? 'bg-kiln-accent hover:bg-emerald-400 text-black shadow-[0_0_15px_rgba(16,185,129,0.2)]' : 'bg-zinc-800 text-zinc-500 cursor-not-allowed'}`}
                        >
                            <Play size={20} fill={selectedSchedule ? "black" : "currentColor"} />
                            {selectedSchedule ? t.dashboard.startFiring : t.dashboard.selectSchedule}
                        </button>
                    ) : (
                        <button 
                            onClick={onStop}
                            className="bg-red-500 hover:bg-red-400 text-white rounded-xl font-bold text-sm flex flex-col items-center justify-center gap-1 transition-all active:scale-95 shadow-[0_0_15px_rgba(239,68,68,0.3)] col-span-2"
                        >
                            <Square size={20} fill="white" />
                            {t.dashboard.stopFiring}
                        </button>
                    )}

                    {isIdle && (
                        <button onClick={onOpenLibrary} className="bg-zinc-800 hover:bg-zinc-700 text-white rounded-xl font-bold text-sm flex flex-col items-center justify-center gap-1 transition-colors active:scale-95 border border-zinc-700">
                            <Thermometer size={20} />
                            {t.nav.schedules}
                        </button>
                    )}
                </div>
            </div>
        </div>
    );
};

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
            <div className="flex justify-between items-center mb-2 shrink-0">
                <div className="flex-1 mr-4">
                    <input 
                        className="text-xl font-bold text-white bg-transparent border-none p-0 w-full focus:ring-0 focus:border-b focus:border-kiln-accent placeholder-zinc-600"
                        value={editingSchedule.name}
                        onChange={(e) => setEditingSchedule({ ...editingSchedule, name: e.target.value })}
                        placeholder={t.schedules.programName}
                    />
                </div>
                <div className="flex items-center gap-2">
                    <button onClick={onSave} className="flex items-center gap-2 px-4 py-2 bg-kiln-accent hover:bg-emerald-400 text-black rounded-xl font-bold transition-colors text-sm">
                        <Settings size={16} /> {t.schedules.saveSchedule}
                    </button>
                    <button onClick={onClose} className="p-2 bg-zinc-800 hover:bg-zinc-700 rounded-xl text-white">
                        <X size={20} />
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
                        <div className="w-8 h-8 rounded-full bg-zinc-800 flex items-center justify-center font-bold text-zinc-500 text-sm shrink-0">
                            {index + 1}
                        </div>
                        
                        <div className="flex-1 grid grid-cols-3 gap-2">
                            <div className="flex flex-col">
                                <label className="text-[10px] text-zinc-500 uppercase font-bold truncate">{t.schedules.rateLabel}</label>
                                <input 
                                    type="number" 
                                    className="bg-transparent text-white font-mono font-bold text-xl w-full border-b border-zinc-700 focus:border-kiln-accent focus:outline-none"
                                    value={step.rate}
                                    onChange={(e) => onEditStep(index, 'rate', e.target.value)}
                                />
                            </div>
                            <div className="flex flex-col">
                                <label className="text-[10px] text-zinc-500 uppercase font-bold truncate">{t.schedules.targetLabel}</label>
                                <input 
                                    type="number" 
                                    className="bg-transparent text-white font-mono font-bold text-xl w-full border-b border-zinc-700 focus:border-kiln-accent focus:outline-none"
                                    value={step.target}
                                    onChange={(e) => onEditStep(index, 'target', e.target.value)}
                                />
                            </div>
                            <div className="flex flex-col">
                                <label className="text-[10px] text-zinc-500 uppercase font-bold truncate">{t.schedules.holdLabel}</label>
                                <input 
                                    type="number" 
                                    className="bg-transparent text-white font-mono font-bold text-xl w-full border-b border-zinc-700 focus:border-kiln-accent focus:outline-none"
                                    value={step.holdTime}
                                    onChange={(e) => onEditStep(index, 'holdTime', e.target.value)}
                                />
                            </div>
                        </div>

                        <button onClick={() => onRemoveStep(index)} className="p-2 text-zinc-600 hover:text-red-500 transition-colors shrink-0">
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
        
        <div className="grid grid-cols-1 gap-2 overflow-y-auto pb-4 pr-2 custom-scrollbar">
            {schedules.map(s => (
                <button 
                    key={s.id}
                    onClick={() => onEdit(s)}
                    className={`p-3 rounded-xl text-left border transition-all active:scale-98 group flex items-center justify-between ${selectedSchedule?.id === s.id ? 'bg-zinc-800 border-kiln-accent ring-1 ring-kiln-accent/50' : 'bg-zinc-900 border-zinc-800 hover:bg-zinc-800'}`}
                >
                    <div className="flex-1 overflow-hidden mr-2">
                        <div className="text-base font-bold text-white truncate">{s.name}</div>
                        <div className="flex gap-2 text-zinc-500 text-[10px]">
                            <span>{s.steps ? s.steps.length : (s.stepsCount || 0)} {t.schedules.segments}</span>
                            <span>??? {t.schedules.custom}</span>
                        </div>
                    </div>
                    
                    {/* Buttons: Select & Delete */}
                    <div className="flex gap-2 shrink-0">
                        <div 
                            className={`w-10 h-10 rounded-full flex items-center justify-center transition-colors ${selectedSchedule?.id === s.id ? 'bg-kiln-accent text-black' : 'bg-zinc-700 text-white group-hover:bg-kiln-accent group-hover:text-black'}`}
                            onClick={(e) => {
                                e.stopPropagation();
                                onSelect(s);
                            }}
                        >
                            <Play size={16} fill="currentColor" />
                        </div>
                    </div>
                </button>
            ))}
            {/* Add New Button */}
             <button 
                onClick={onNew}
                className="p-3 rounded-xl border border-dashed border-zinc-700 flex items-center justify-center gap-2 text-zinc-500 hover:text-white hover:border-zinc-500 transition-colors"
            >
                <div className="w-8 h-8 rounded-full bg-zinc-800 flex items-center justify-center">
                    <Settings size={16} />
                </div>
                <span className="font-medium text-sm">{t.schedules.newSchedule}</span>
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
                .then(data => {
                    // Ensure steps array exists or use stepsCount
                    const processed = data.map((s: any) => ({
                        ...s,
                        steps: s.steps || [],
                        stepsCount: s.stepsCount || (s.steps ? s.steps.length : 0)
                    }));
                    setSchedules(processed);
                })
                .catch(e => console.error(e));
        }
    }, [view]);

    const handleStartFiring = async () => {
        if (!selectedSchedule) {
            setView('SCHEDULES');
            return;
        }
        try {
            // Ensure we send the full object structure expected by backend
            await fetch(`${API_BASE_URL}/start`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ schedule: selectedSchedule })
            });
            setView('DASHBOARD');
        } catch (e) {
            alert("Error starting");
        }
    };

    const handleSaveSchedule = async () => {
        if (!editingSchedule) return;
        
        // Ensure name is safe for file system
        const safeName = editingSchedule.name.replace(/ /g, "_").replace(/\//g, "");
        const scheduleToSave = {
            ...editingSchedule,
            name: safeName,
            id: safeName // Ensure ID matches name for consistency
        };

        try {
            await fetch(`${API_BASE_URL}/schedules`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(scheduleToSave)
            });
            // Reload list
            const res = await fetch(`${API_BASE_URL}/schedules`);
            const data = await res.json();
            
            // Process data same as initial load
            const processed = data.map((s: any) => ({
                ...s,
                steps: s.steps || [],
                stepsCount: s.stepsCount || (s.steps ? s.steps.length : 0)
            }));
            setSchedules(processed);
            
            // If this was a new schedule, select it
            if (!selectedSchedule || selectedSchedule.id === editingSchedule.id) {
                setSelectedSchedule(scheduleToSave);
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

    const handleStopFiring = () => {
        setShowStopConfirm(true);
    };

    const confirmStop = async () => {
        try {
            await fetch(`${API_BASE_URL}/stop`, { method: 'POST' });
            setShowStopConfirm(false);
        } catch (e) {
            console.error("Error stopping");
        }
    };

    const handleSelectSchedule = (s: Schedule) => {
        setSelectedSchedule(s);
        setView('DASHBOARD');
    };

    const handleNewSchedule = () => {
        // Just use a random string for temporary ID
        const tempId = Math.random().toString(36).substring(2, 15);
        
        const newSchedule = {
            id: tempId,
            name: t.schedules.newSchedule, // Use localized name
            steps: []
        };
        setEditingSchedule(newSchedule);
        setView('EDITOR');
    };

    const handleEditSchedule = async (s: Schedule) => {
        // Fetch full details if steps are missing
        if (!s.steps || s.steps.length === 0) {
            try {
                const res = await fetch(`${API_BASE_URL}/schedules?name=${encodeURIComponent(s.name)}`);
                if (res.ok) {
                    const detailed = await res.json();
                    setEditingSchedule({ ...s, steps: detailed.steps || [] });
                } else {
                    setEditingSchedule({ ...s, steps: [] });
                }
            } catch (e) {
                setEditingSchedule({ ...s, steps: [] });
            }
        } else {
            setEditingSchedule(s);
        }
        setView('EDITOR');
    };

    return (
        <div className="flex flex-col items-center justify-center min-h-screen bg-zinc-950 p-8 select-none">
            <h2 className="text-zinc-500 mb-4 font-mono text-sm">Trae Controller 4.0" Simulator (480x320)</h2>
            
            {/* The Screen Container */}
            <div 
                className="relative bg-black overflow-hidden shadow-2xl border-[12px] border-zinc-900 rounded-[2rem]"
                style={{ width: '480px', height: '320px' }}
            >
                {/* Header Bar */}
                <div className="h-10 bg-zinc-900/80 backdrop-blur-md border-b border-zinc-800 flex justify-between items-center px-4 absolute top-0 left-0 right-0 z-10">
                    <div className="flex items-center gap-2">
                        <Activity className="text-kiln-accent" size={16} />
                        <span className="text-sm font-bold text-white tracking-wider">KILN PRO</span>
                    </div>
                    <div className="flex items-center gap-3 text-zinc-400 font-mono text-xs">
                        <button 
                            onClick={() => setLanguage(language === 'en' ? 'ua' : 'en')} 
                            className="px-2 py-0.5 bg-zinc-800 rounded hover:bg-zinc-700 text-white font-bold uppercase transition-colors text-[10px] tracking-wider"
                        >
                            {language}
                        </button>
                        <span>{currentTime}</span>
                        <Wifi size={14} className={status.status !== 'ERROR' ? "text-emerald-500" : "text-zinc-600"} />
                    </div>
                </div>

                {/* Main Content Area (With Padding for Header) */}
                <div className="pt-10 h-full bg-black text-white relative text-sm">
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
