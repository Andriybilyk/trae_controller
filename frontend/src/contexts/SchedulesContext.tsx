import React, { createContext, useContext, useState, useEffect, useCallback } from 'react';
import toast from 'react-hot-toast';
import { deleteJson, getJson, postJson } from '../api/http';
import { toastApiError } from '../api/notify';

// --- Types ---
export interface Step {
    id: number;
    type: 'ramp' | 'hold';
    target: number;
    rate?: number;
    holdTime?: number;
    fan?: boolean;
}

export interface Schedule {
    id: string;
    name: string;
    steps: Step[];
    favorite?: boolean;
    type?: string;
    stepsCount?: number;
}

interface SchedulesContextType {
    schedules: Schedule[];
    isLoading: boolean;
    refreshSchedules: () => Promise<void>;
    saveSchedule: (schedule: Schedule) => Promise<boolean>;
    deleteSchedule: (schedule: Schedule) => Promise<boolean>;
    getScheduleDetails: (schedule: Schedule) => Promise<Schedule | null>;
}

const SchedulesContext = createContext<SchedulesContextType | undefined>(undefined);

export const SchedulesProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
    const [schedules, setSchedules] = useState<Schedule[]>([]);
    const [isLoading, setIsLoading] = useState(false);

    // 1. Fetch List
    const refreshSchedules = useCallback(async () => {
        try {
            const res = await getJson<any[]>(`/schedules?t=${Date.now()}`);
            if (res.ok && Array.isArray(res.data)) {
                const data = res.data;
                if (Array.isArray(data)) {
                    setSchedules(prev => {
                        // Create a map of existing schedules to preserve full details if already loaded
                        const existingMap = new Map(prev.map(s => [s.name, s]));
                        
                        const processed = data.map((s: any) => {
                            const existing = existingMap.get(s.name);
                            
                            // CRITICAL FIX: The list endpoint returns `stepsCount`.
                            // If we have local steps, check if they match the count.
                            // If the server says there are steps (stepsCount > 0), but we have none or wrong count,
                            // we should probably trust the server's count and maybe clear our stale steps 
                            // to force a re-fetch when clicked, OR keep existing if it looks valid.
                            
                            let steps = s.steps || [];
                            
                            if (existing && existing.steps && existing.steps.length > 0) {
                                // If server list doesn't return steps (it usually doesn't), keep ours.
                                if (steps.length === 0) {
                                    steps = existing.steps;
                                }
                            }

                            return {
                                ...s,
                                id: s.name, // ID is always Name
                                name: s.name,
                                steps: steps,
                                stepsCount: s.stepsCount !== undefined ? s.stepsCount : steps.length
                            };
                        }).sort((a: any, b: any) => (a.name || "").localeCompare(b.name || ""));
                        
                        return processed;
                    });
                }
            } else if (!res.ok) {
                toastApiError(res, "Connection Error");
            }
        } catch (e) {
            console.error("Fetch Error:", e);
            toast.error("Connection Error");
        }
    }, []);

    // 2. Load Details (Lazy Load)
    const getScheduleDetails = async (schedule: Schedule): Promise<Schedule | null> => {
        try {
            // ALWAYS fetch details to ensure we have the latest data
            // removing the "if steps exist" optimization because it causes stale data issues
            
            const res = await getJson<any>(`/schedules?name=${encodeURIComponent(schedule.name)}&t=${Date.now()}`);
            if (res.ok && res.data) {
                const fullData = res.data;
                const fullSchedule = { 
                    ...schedule, 
                    ...fullData,
                    id: fullData.name || schedule.name,
                    name: fullData.name || schedule.name,
                    stepsCount: fullData.steps ? fullData.steps.length : (fullData.stepsCount || 0)
                };
                
                // Update in local list to cache it
                setSchedules(prev => prev.map(s => s.name === schedule.name ? fullSchedule : s));
                
                return fullSchedule;
            }
        } catch (e) {
            console.error("Details Error:", e);
        }
        return null;
    };

    // 3. Save
    const saveSchedule = async (schedule: Schedule): Promise<boolean> => {
        try {
            // Ensure ID is set (use name as ID)
            const scheduleToSave = {
                ...schedule,
                id: schedule.name, // Enforce ID = Name
                stepsCount: schedule.steps ? schedule.steps.length : 0 // Ensure count is updated
            };

            const res = await postJson<any>('/schedules', scheduleToSave);
            
            if (res.ok) {
                // Optimistically update local state immediately
                setSchedules(prev => {
                    const exists = prev.some(s => s.name === scheduleToSave.name);
                    let newSchedules;
                    
                    if (exists) {
                        newSchedules = prev.map(s => {
                            if (s.name === scheduleToSave.name) {
                                return { 
                                    ...scheduleToSave, 
                                    steps: scheduleToSave.steps || [],
                                    stepsCount: scheduleToSave.steps ? scheduleToSave.steps.length : 0
                                };
                            }
                            return s;
                        });
                    } else {
                        newSchedules = [...prev, { 
                            ...scheduleToSave, 
                            steps: scheduleToSave.steps || [],
                            stepsCount: scheduleToSave.steps ? scheduleToSave.steps.length : 0
                        }];
                    }
                    return newSchedules.sort((a, b) => (a.name || "").localeCompare(b.name || ""));
                });
                
                // Force a background refresh to ensure server state is synced
                setTimeout(() => {
                    getScheduleDetails(scheduleToSave); 
                }, 500);

                toast.success("Saved");
                return true;
            } else {
                toastApiError(res, "Save Failed");
            }
        } catch (e) {
            console.error("Save Error:", e);
            toast.error("Save Error");
        }
        return false;
    };

    // 4. Delete
    const deleteSchedule = async (schedule: Schedule): Promise<boolean> => {
        try {
            // Fallback to ID if Name is missing, or vice versa
            const targetName = schedule.name || schedule.id;
            
            if (!targetName) {
                console.error("Delete Error: Missing Name/ID", schedule);
                toast.error("Error: Missing Schedule Name");
                return false;
            }

            const res = await deleteJson<any>(`/schedules?name=${encodeURIComponent(targetName)}`);
            
            if (res.ok || res.status === 404) {
                setSchedules(prev => prev.filter(s => (s.name || s.id) !== targetName));
                toast.success("Deleted");
                return true;
            } else {
                toastApiError(res, "Delete Failed");
            }
        } catch (e) {
            console.error("Delete Error:", e);
            toast.error("Delete Error");
        }
        return false;
    };

    // Initial Load
    useEffect(() => {
        refreshSchedules();
    }, [refreshSchedules]);

    // Cross-device sync: listen for schedules_changed over WS
    useEffect(() => {
        let ws: WebSocket | null = null;
        let stopped = false;
        let retry = 0;
        let lastTrigger = 0;
        let lastSchedulesRev = -1;

        const connect = () => {
            if (stopped) return;

            const proto = window.location.protocol === 'https:' ? 'wss' : 'ws';
            const url = `${proto}://${window.location.host}/ws`;

            try {
                ws = new WebSocket(url);
            } catch (e) {
                scheduleReconnect();
                return;
            }

            ws.onopen = () => {
                retry = 0;
            };

            ws.onmessage = (ev) => {
                try {
                    const msg = JSON.parse(ev.data);
                    if (msg) {
                        try {
                            window.dispatchEvent(new CustomEvent('kiln_ws_message', { detail: msg }));
                        } catch {}
                    }
                    if (msg && msg.event) {
                        try {
                            window.dispatchEvent(new CustomEvent('kiln_ws', { detail: msg }));
                        } catch {}
                    }

                    if (msg && msg.event === 'schedules_changed') {
                        const now = Date.now();
                        if (now - lastTrigger < 250) return;
                        lastTrigger = now;
                        refreshSchedules();
                    }

                    // Fallback sync path: status frames carry schedules_rev.
                    // If explicit schedules_changed event was missed, revision change still triggers refresh.
                    if (msg && typeof msg.schedules_rev === 'number') {
                        const rev = Number(msg.schedules_rev);
                        if (Number.isFinite(rev) && rev >= 0) {
                            if (lastSchedulesRev < 0) {
                                lastSchedulesRev = rev;
                            } else if (rev !== lastSchedulesRev) {
                                lastSchedulesRev = rev;
                                const now = Date.now();
                                if (now - lastTrigger >= 250) {
                                    lastTrigger = now;
                                    refreshSchedules();
                                }
                            }
                        }
                    }
                } catch {
                    // ignore non-JSON / state frames
                }
            };

            ws.onclose = () => {
                scheduleReconnect();
            };

            ws.onerror = () => {
                try { ws?.close(); } catch {}
            };
        };

        const scheduleReconnect = () => {
            if (stopped) return;
            const delay = Math.min(5000, 250 * Math.pow(2, retry));
            retry = Math.min(6, retry + 1);
            setTimeout(connect, delay);
        };

        connect();
        return () => {
            stopped = true;
            try { ws?.close(); } catch {}
        };
    }, [refreshSchedules]);

    return (
        <SchedulesContext.Provider value={{ schedules, isLoading, refreshSchedules, saveSchedule, deleteSchedule, getScheduleDetails }}>
            {children}
        </SchedulesContext.Provider>
    );
};

export const useSchedules = () => {
    const context = useContext(SchedulesContext);
    if (!context) throw new Error("useSchedules must be used within SchedulesProvider");
    return context;
};
