
import React, { useRef, useState, useEffect } from 'react';
import { useLanguage } from '../contexts/LanguageContext';
import { Clock, Thermometer, Hash, AlertTriangle, CheckCircle, XCircle, Loader, X, Trash2 } from 'lucide-react';
import { Line } from 'react-chartjs-2';
import { Chart as ChartJS, CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler } from 'chart.js';
import toast from 'react-hot-toast';
import { deleteJson, getJson } from '../api/http';
import { ConfirmModal } from './ConfirmModal';

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, Title, Tooltip, Legend, Filler);

const formatHourMinuteLabel = (hoursValue: number, hourSuffix: string, minSuffix: string) => {
    if (!Number.isFinite(hoursValue)) return '';
    const totalMinutes = Math.max(0, Math.round(hoursValue * 60));
    const hours = Math.floor(totalMinutes / 60);
    const minutes = totalMinutes % 60;
    if (minutes === 0) return `${hours}${hourSuffix}`;
    return `${hours}${hourSuffix} ${minutes}${minSuffix}`;
};

const historyTargetBreakpointLabelsPlugin = {
    id: 'historyTargetBreakpointLabels',
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
        ctx.setLineDash([]);
        ctx.lineWidth = 1;
        ctx.strokeStyle = 'rgba(113, 113, 122, 0.6)';
        ctx.fillStyle = 'rgba(241, 245, 249, 1)';
        ctx.font = '700 11px Inter, system-ui, sans-serif';

        for (let i = 1; i < points.length; i += 1) {
            const point = points[i];
            if (!point || typeof point.x !== 'number' || typeof point.y !== 'number') continue;
            const x = xScale.getPixelForValue(point.x);
            const y = yScale.getPixelForValue(point.y);
            if (x < chartArea.left || x > chartArea.right || y < chartArea.top || y > chartArea.bottom) continue;

            const tempLabel = `${Math.round(point.y)}°C`;
            ctx.textBaseline = 'middle';
            const labelY = Math.min(Math.max(y - 10, chartArea.top + 10), chartArea.bottom - 10);
            const tempLabelWidth = ctx.measureText(tempLabel).width + 8;
            const rightEdge = x - 8;
            const leftEdge = Math.max(chartArea.left + 2, rightEdge - tempLabelWidth);
            ctx.fillStyle = 'rgba(2, 6, 23, 0.92)';
            ctx.fillRect(leftEdge, labelY - 6, tempLabelWidth, 12);
            ctx.fillStyle = 'rgba(241, 245, 249, 1)';
            ctx.textAlign = 'right';
            ctx.fillText(tempLabel, leftEdge + tempLabelWidth - 3, labelY);
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
        const unique = safe.length > 0 ? Array.from(new Set(safe)) : [0];
        const min = Number(scale.min ?? 0);
        const max = Number(scale.max ?? min);
        const inView = unique.filter((v: number) => v >= min - 1e-6 && v <= max + 1e-6);
        const base = inView.length > 0 ? inView : [min, max].filter((v, i, arr) => i === 0 || v !== arr[i - 1]);

        const maxTicks = 8;
        if (base.length <= maxTicks) {
            scale.ticks = base.map((v: number) => ({ value: v }));
            return;
        }

        const stride = Math.ceil(base.length / maxTicks);
        const reduced: number[] = [];
        for (let i = 0; i < base.length; i += stride) reduced.push(base[i]);
        const last = base[base.length - 1];
        if (reduced[reduced.length - 1] !== last) reduced.push(last);
        scale.ticks = reduced.map((v: number) => ({ value: v }));
    }
};

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

interface FiringCurvePoint {
    t: number;
    temp: number;
}

interface FiringDetail {
    summary: FiringSummary;
    data: FiringDataPoint[];
    planned?: FiringCurvePoint[];
    actual?: FiringCurvePoint[];
}

const formatTemp = (t: number | null | undefined) => {
    if (t === null || t === undefined || isNaN(t)) return '--';
    return `${t.toFixed(1)}°C`;
};

const safePct = (num: number, den: number) => {
    if (!den) return 0;
    return Math.max(0, Math.min(100, Math.round((num / den) * 100)));
};

// --- Main Component ---
const History = () => {
    const { t } = useLanguage();
    const [history, setHistory] = useState<FiringSummary[]>([]);
    const [loading, setLoading] = useState(true);
    const [error, setError] = useState<string | null>(null);
    
    const [selectedFiring, setSelectedFiring] = useState<FiringDetail | null>(null);
    const [zoomX, setZoomX] = useState(1);
    const [panX, setPanX] = useState(0);
    const [isDraggingPan, setIsDraggingPan] = useState(false);
    const chartPanRef = useRef<HTMLDivElement | null>(null);
    const dragRef = useRef<{ active: boolean; startClientX: number; startPan: number }>({
        active: false,
        startClientX: 0,
        startPan: 0
    });
    const [isDetailLoading, setIsDetailLoading] = useState(false);
    const [clearBusy, setClearBusy] = useState(false);
    const [showClearConfirm, setShowClearConfirm] = useState(false);

    const fetchHistory = async () => {
        setLoading(true);
        setError(null);
        try {
            const res = await getJson<FiringSummary[]>('/history');
            if (!res.ok || !Array.isArray(res.data)) {
                setError(res.message || 'Failed to fetch history data.');
                return;
            }
            setHistory(res.data);
        } catch (err) {
            setError(err instanceof Error ? err.message : String(err));
        } finally {
            setLoading(false);
        }
    };

    useEffect(() => {
        fetchHistory();
    }, []);

    const confirmClearHistory = async () => {
        const tr = t.history as any;
        setClearBusy(true);
        try {
            const res = await deleteJson<any>('/history');
            if (!res.ok) throw new Error(res.message || tr.clearHistoryFailed || 'Failed to clear history');
            toast.success(tr.historyCleared || 'History cleared');
            setSelectedFiring(null);
            setShowClearConfirm(false);
            await fetchHistory();
        } catch (err) {
            toast.error(err instanceof Error ? err.message : (tr.clearHistoryFailed || 'Failed to clear history'));
        } finally {
            setClearBusy(false);
        }
    };

    const handleSelectFiring = async (id: string) => {
        setIsDetailLoading(true);
        try {
            const res = await getJson<FiringDetail>(`/history/${id}`);
            if (!res.ok || !res.data) throw new Error(res.message || 'Failed to load firing details.');
            const data: FiringDetail = res.data;
            
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
                setZoomX(1);
                setPanX(0);
                setSelectedFiring(data);
            }
        } catch (err) {
            console.error(err);
            toast.error(err instanceof Error ? err.message : 'Failed to load firing details');
        } finally {
            setIsDetailLoading(false);
        }
    };

    const formatDuration = (seconds: number) => {
        const h = Math.floor(seconds / 3600);
        const m = Math.floor((seconds % 3600) / 60);
        return `${h > 0 ? `${h}h ` : ''}${m}m`;
    };

    const deriveStats = (detail: FiringDetail | null) => {
        if (!detail) return null;
        const data = detail.data || [];
        const first = data.length ? data[0] : null;
        const last = data.length ? data[data.length - 1] : null;

        const temps = data.map(p => p.temp).filter(v => typeof v === 'number' && !isNaN(v));
        const targets = data.map(p => p.target).filter(v => typeof v === 'number' && !isNaN(v));

        const startTemp = first ? first.temp : null;
        const endTemp = last ? last.temp : null;
        const peakTemp = temps.length ? Math.max(...temps) : (detail.summary.peakTemp ?? null);
        const maxTarget = targets.length ? Math.max(...targets) : null;
        const rise = (peakTemp !== null && startTemp !== null && !isNaN(startTemp)) ? (peakTemp - startTemp) : null;

        return { startTemp, endTemp, peakTemp, maxTarget, rise };
    };

    const buildPlanFromDetail = (detail: FiringDetail) => {
        const planned = Array.isArray(detail?.planned) ? detail.planned : [];
        if (planned.length < 2) return [] as { x: number; y: number }[];
        return planned
            .map((p) => ({ x: Number(p?.t) / 60, y: Number(p?.temp) }))
            .filter((p) => Number.isFinite(p.x) && Number.isFinite(p.y))
            .sort((a, b) => a.x - b.x);
    };

    const toHoursFromStart = (ts: number | undefined, start: number | undefined) => {
        if (!Number.isFinite(Number(ts)) || !Number.isFinite(Number(start))) return 0;
        return Math.max(0, (Number(ts) - Number(start)) / 3600000);
    };

    const getHistoryChartModel = (detail: FiringDetail | null) => {
        if (!detail || !detail.summary || !Array.isArray(detail.data)) {
            return { target: [], actual: [], ticks: [0], xMax: 0 };
        }

        const start = Number(detail.summary.startTime);
        const samples = detail.data
            .filter((p) => p && Number.isFinite(Number(p.timestamp)))
            .map((p) => ({
                x: toHoursFromStart(Number(p.timestamp), start),
                target: Number(p.target),
                temp: Number(p.temp)
            }))
            .sort((a, b) => a.x - b.x);

        const actual = samples
            .filter((p) => Number.isFinite(p.temp))
            .map((p) => ({ x: p.x, y: p.temp }));

        const detailPlan = buildPlanFromDetail(detail);
        const targetRaw = (detailPlan.length > 1)
            ? detailPlan
            : samples
                .filter((p) => Number.isFinite(p.target))
                .map((p) => ({ x: p.x, y: p.target }));

        const target: { x: number; y: number }[] = [];
        for (let i = 0; i < targetRaw.length; i += 1) {
            const point = targetRaw[i];
            const prev = i > 0 ? targetRaw[i - 1] : null;
            const next = i + 1 < targetRaw.length ? targetRaw[i + 1] : null;
            const firstOrLast = i === 0 || i === targetRaw.length - 1;
            const yChangedFromPrev = !!prev && Math.abs(point.y - prev.y) >= 0.1;
            const yChangedToNext = !!next && Math.abs(next.y - point.y) >= 0.1;
            // Keep breakpoints from both sides so hold plateaus keep exact start/end shelves.
            if (firstOrLast || yChangedFromPrev || yChangedToNext) {
                target.push(point);
            }
        }

        const ticks = Array.from(new Set(target.map((p) => Number(p.x.toFixed(4))))).sort((a, b) => a - b);
        const targetXMax = target.length > 0 ? target[target.length - 1].x : 0;
        const actualXMax = actual.length > 0 ? actual[actual.length - 1].x : 0;
        return {
            target,
            actual,
            ticks: ticks.length > 0 ? ticks : [0],
            xMax: Math.max(targetXMax, actualXMax),
            planXMax: targetXMax
        };
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
    const chartModel = getHistoryChartModel(selectedFiring);
    const fullPlanXMax = Math.max(0, Number((chartModel as any).planXMax ?? chartModel.xMax ?? 0));
    const baseXMax = fullPlanXMax > 0 ? fullPlanXMax : Math.max(0.1, chartModel.xMax);
    const clampedZoom = Math.max(1, Math.min(20, zoomX));
    const viewWidth = Math.max(baseXMax / clampedZoom, 0.02);
    const maxPanStart = Math.max(0, baseXMax - viewWidth);
    const viewMin = maxPanStart * Math.max(0, Math.min(1, panX));
    const viewMax = Math.min(baseXMax, viewMin + viewWidth);
    const historyChartOptions = {
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
                    label: (context: any) => `${context.dataset.label}: ${Number(context?.parsed?.y ?? context?.raw?.y ?? 0).toFixed(0)}°C`
                }
            },
            historyTargetBreakpointLabels: {
                datasetIndex: 0
            },
            dynamicXAxisTicks: {
                values: chartModel.ticks
            }
        },
        scales: {
            x: {
                type: 'linear' as const,
                min: viewMin,
                max: viewMax,
                grid: { color: '#27272a' },
                ticks: {
                    color: '#71717a',
                    autoSkip: true,
                    maxTicksLimit: 8,
                    maxRotation: 0,
                    callback: (value: any) => formatHourMinuteLabel(Number(value), t.dashboard.hourSuffix || 'h', t.dashboard.minSuffix || 'm')
                }
            },
            y: {
                min: 0,
                grid: { color: '#27272a' },
                ticks: { color: '#71717a' }
            }
        }
    };

    const beginDragPan = (clientX: number) => {
        if (clampedZoom <= 1.01) return;
        dragRef.current = { active: true, startClientX: clientX, startPan: panX };
        setIsDraggingPan(true);
    };

    const moveDragPan = (clientX: number) => {
        const drag = dragRef.current;
        if (!drag.active || !chartPanRef.current || maxPanStart <= 0) return;
        const width = Math.max(1, chartPanRef.current.clientWidth);
        const deltaPx = clientX - drag.startClientX;
        const deltaHours = -(deltaPx / width) * viewWidth;
        const startHours = drag.startPan * maxPanStart;
        const nextStart = Math.max(0, Math.min(maxPanStart, startHours + deltaHours));
        setPanX(nextStart / maxPanStart);
    };

    const endDragPan = () => {
        dragRef.current.active = false;
        setIsDraggingPan(false);
    };

    if (loading) {
        return <div className="flex justify-center items-center h-64"><Loader className="animate-spin text-kiln-accent" size={48} /></div>;
    }

    if (error) {
        return <div className="text-center text-red-400 bg-red-500/10 p-8 rounded-xl"><h2 className="font-bold text-lg">{t.history.error}</h2><p>{error}</p></div>;
    }

    return (
        <div className="max-w-7xl mx-auto">
            <div className="mb-6 flex items-center justify-between gap-3">
                <h1 className="text-3xl font-bold text-white">{t.history.title}</h1>
                <button
                    onClick={() => setShowClearConfirm(true)}
                    disabled={clearBusy || loading}
                    className="px-3 py-2 rounded-lg border border-red-500/30 text-red-300 bg-red-500/10 hover:bg-red-500/20 disabled:opacity-50 disabled:cursor-not-allowed"
                >
                    {clearBusy ? '...' : (((t.history as any).clearHistory) || 'Clear History')}
                </button>
            </div>

            <ConfirmModal
                open={showClearConfirm}
                title={((t.history as any).clearHistory) || 'Clear History'}
                description={((t.history as any).clearHistoryConfirm) || 'Delete all firing history?'}
                confirmText={((t.history as any).clearHistory) || 'Clear History'}
                cancelText={t.schedules.cancel}
                icon={<Trash2 size={40} />}
                variant="danger"
                busy={clearBusy}
                onConfirm={confirmClearHistory}
                onCancel={() => setShowClearConfirm(false)}
            />

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
                                        <p className="text-xs text-zinc-400">{item.startTime > 946684800000 ? new Date(item.startTime).toLocaleString() : 'No RTC timestamp'}</p>
                                    </div>
                                    <div className="flex items-center gap-2 text-sm text-zinc-300"><Clock size={16} className="text-zinc-500" /><span>{formatDuration(item.duration)}</span></div>
                                    <div className="flex items-center gap-2 text-sm text-zinc-300"><Thermometer size={16} className="text-zinc-500" /><span>{typeof item.peakTemp === 'number' ? `${item.peakTemp.toFixed(0)}°C ${t.history.peakTemp}` : '--'}</span></div>
                                    <div className="flex justify-end">
                                        <StatusBadge status={item.status} />
                                    </div>
                                </div>
                                <div className="mt-2 flex items-center gap-2 text-xs text-zinc-500">
                                    <Hash size={14} />
                                    <span>{t.history.steps}: {item.completedSteps} / {item.totalSteps} ({safePct(item.completedSteps, item.totalSteps)}%)</span>
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
                                        <p className="text-sm text-zinc-400">{selectedFiring.summary.startTime > 946684800000 ? new Date(selectedFiring.summary.startTime).toLocaleString() : 'No RTC timestamp (uptime-based session)'}</p>
                                        <p className="text-xs text-zinc-500 mt-1">{t.history.steps}: {selectedFiring.summary.completedSteps} / {selectedFiring.summary.totalSteps}</p>
                                    </div>
                                        <button onClick={() => { setSelectedFiring(null); }} className="p-2 rounded-full text-zinc-400 hover:bg-zinc-800 hover:text-white"><X size={20} /></button>
                                </div>
                                <div className="p-4 md:p-6 flex-1 overflow-hidden">
                                    {(() => {
                                        const s = deriveStats(selectedFiring);
                                        const pct = safePct(selectedFiring.summary.completedSteps, selectedFiring.summary.totalSteps);
                                        return (
                                            <div className="grid grid-cols-2 md:grid-cols-3 gap-3 mb-4">
                                                <div className="bg-black/20 border border-zinc-800 rounded-xl p-3">
                                                    <div className="text-[10px] uppercase font-bold text-zinc-500">{t.history.duration}</div>
                                                    <div className="text-white font-mono text-xl">{formatDuration(selectedFiring.summary.duration)}</div>
                                                </div>
                                                <div className="bg-black/20 border border-zinc-800 rounded-xl p-3">
                                                    <div className="text-[10px] uppercase font-bold text-zinc-500">{t.history.completion}</div>
                                                    <div className="text-white font-mono text-xl">{pct}%</div>
                                                    <div className="text-zinc-500 text-xs">{selectedFiring.summary.completedSteps}/{selectedFiring.summary.totalSteps}</div>
                                                </div>
                                                <div className="bg-black/20 border border-zinc-800 rounded-xl p-3">
                                                    <div className="text-[10px] uppercase font-bold text-zinc-500">{t.history.peakTemp}</div>
                                                    <div className="text-white font-mono text-xl">{formatTemp(s?.peakTemp ?? selectedFiring.summary.peakTemp)}</div>
                                                </div>
                                                <div className="bg-black/20 border border-zinc-800 rounded-xl p-3">
                                                    <div className="text-[10px] uppercase font-bold text-zinc-500">{t.history.startTemp}</div>
                                                    <div className="text-white font-mono text-xl">{formatTemp(s?.startTemp ?? null)}</div>
                                                </div>
                                                <div className="bg-black/20 border border-zinc-800 rounded-xl p-3">
                                                    <div className="text-[10px] uppercase font-bold text-zinc-500">{t.history.endTemp}</div>
                                                    <div className="text-white font-mono text-xl">{formatTemp(s?.endTemp ?? null)}</div>
                                                </div>
                                                <div className="bg-black/20 border border-zinc-800 rounded-xl p-3">
                                                    <div className="text-[10px] uppercase font-bold text-zinc-500">{t.history.tempRise}</div>
                                                    <div className="text-white font-mono text-xl">{s?.rise === null || s?.rise === undefined || isNaN(s.rise) ? '--' : `${s.rise.toFixed(1)}°C`}</div>
                                                    <div className="text-zinc-500 text-xs">{t.history.maxTarget}: {formatTemp(s?.maxTarget ?? null)}</div>
                                                </div>
                                            </div>
                                        );
                                    })()}

                                    {selectedFiring.data && selectedFiring.data.length > 1 ? (
                                        <div className="h-[320px]">
                                        <div className="mb-3 flex items-center gap-2">
                                            <button
                                                onClick={() => setZoomX((z) => Math.min(20, Number((z * 1.5).toFixed(3))))}
                                                className="px-2 py-1 text-xs rounded border border-zinc-700 text-zinc-200 hover:bg-zinc-800"
                                            >
                                                Zoom +
                                            </button>
                                            <button
                                                onClick={() => setZoomX((z) => Math.max(1, Number((z / 1.5).toFixed(3))))}
                                                className="px-2 py-1 text-xs rounded border border-zinc-700 text-zinc-200 hover:bg-zinc-800"
                                            >
                                                Zoom -
                                            </button>
                                            <button
                                                onClick={() => { setZoomX(1); setPanX(0); }}
                                                className="px-2 py-1 text-xs rounded border border-zinc-700 text-zinc-200 hover:bg-zinc-800"
                                            >
                                                Reset
                                            </button>
                                            <div className="ml-2 text-xs text-zinc-500">
                                                {`x${clampedZoom.toFixed(1)}`}
                                            </div>
                                            {clampedZoom > 1.01 && (
                                                <div className="text-xs text-zinc-500">Drag chart to pan</div>
                                            )}
                                        </div>
                                        <div
                                            ref={chartPanRef}
                                            className={`h-[275px] ${clampedZoom > 1.01 ? (isDraggingPan ? 'cursor-grabbing' : 'cursor-grab') : 'cursor-default'}`}
                                            style={{ touchAction: clampedZoom > 1.01 ? 'none' as const : 'auto' as const }}
                                            onMouseDown={(e) => beginDragPan(e.clientX)}
                                            onMouseMove={(e) => moveDragPan(e.clientX)}
                                            onMouseUp={endDragPan}
                                            onMouseLeave={endDragPan}
                                            onTouchStart={(e) => beginDragPan(e.touches[0]?.clientX ?? 0)}
                                            onTouchMove={(e) => {
                                                moveDragPan(e.touches[0]?.clientX ?? 0);
                                                if (clampedZoom > 1.01) e.preventDefault();
                                            }}
                                            onTouchEnd={endDragPan}
                                        >
                                        <Line options={historyChartOptions} plugins={[historyTargetBreakpointLabelsPlugin, dynamicXAxisTicksPlugin]} data={{
                                            datasets: [
                                                {
                                                    label: t.history.targetTemp,
                                                    data: chartModel.target,
                                                    borderColor: '#6366f1',
                                                    borderDash: [5, 5],
                                                    pointRadius: 4,
                                                    pointHoverRadius: 5,
                                                    pointBorderWidth: 1,
                                                    pointBackgroundColor: '#0f1115',
                                                    pointBorderColor: '#818cf8',
                                                    tension: 0,
                                                    fill: false
                                                },
                                                {
                                                    label: t.history.currentTemp,
                                                    data: chartModel.actual,
                                                    borderColor: '#10b981',
                                                    backgroundColor: 'rgba(16, 185, 129, 0.1)',
                                                    fill: true,
                                                    pointRadius: 0,
                                                    tension: 0.4
                                                }
                                            ]
                                        }} />
                                        </div>
                                        </div>
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
                                 <button onClick={() => { setSelectedFiring(null); }} className="mt-4 px-4 py-2 bg-zinc-800 rounded-md text-white">{t.dashboard.done}</button>
                             </div>
                        )}
                    </div>
                </div>
            )}
        </div>
    );
};

export default History;
