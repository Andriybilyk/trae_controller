
import fs from 'fs/promises';
import path from 'path';

const HISTORY_DIR = path.join(__dirname, '..', '..', 'data', 'history');

interface FiringDataPoint {
    timestamp: number;
    temp: number;
    target: number;
    output: number;
}

let currentFiringSession: {
    startTime: number;
    scheduleName: string;
    totalSteps: number;
    lastKnownStep: number;
    data: FiringDataPoint[];
} | null = null;

let isLogging = false;

// Ensure history directory exists
const ensureHistoryDir = async () => {
    try {
        await fs.mkdir(HISTORY_DIR, { recursive: true });
    } catch (error) {
        console.error('Error creating history directory:', error);
    }
};

ensureHistoryDir();

export const startLogging = (scheduleName: string, totalSteps: number) => {
    if (isLogging) {
        console.warn('Logging is already active. Please stop the current session first.');
        return;
    }
    
    const startTime = Date.now();
    currentFiringSession = {
        startTime,
        scheduleName: scheduleName || 'Custom Firing',
        totalSteps: totalSteps || 0,
        lastKnownStep: 0,
        data: [],
    };
    isLogging = true;
    console.log(`[History] Started logging for schedule: ${scheduleName}`);
};

export const logDataPoint = (data: any) => {
    if (!isLogging || !currentFiringSession) {
        return;
    }

    // Update last known step
    if (data.step) {
        currentFiringSession.lastKnownStep = data.step;
    }

    const point: FiringDataPoint = {
        timestamp: Date.now(),
        temp: data.temp,
        target: data.target,
        output: data.output,
    };
    currentFiringSession.data.push(point);
};

export const stopLogging = async (finalStatus: 'COMPLETED' | 'ERROR' | 'STOPPED') => {
    if (!isLogging || !currentFiringSession) {
        return;
    }

    const endTime = Date.now();
    const duration = Math.round((endTime - currentFiringSession.startTime) / 1000); // in seconds

    const summary = {
        id: `firing_${currentFiringSession.startTime}`,
        scheduleName: currentFiringSession.scheduleName,
        startTime: currentFiringSession.startTime,
        endTime,
        duration,
        status: finalStatus,
        peakTemp: currentFiringSession.data.length > 0 ? Math.max(...currentFiringSession.data.map(p => p.temp)) : null,
        totalSteps: currentFiringSession.totalSteps,
        completedSteps: finalStatus === 'COMPLETED' ? currentFiringSession.totalSteps : currentFiringSession.lastKnownStep,
    };

    const detailedLog = {
        summary,
        data: currentFiringSession.data,
    };

    const filename = `${summary.id}.json`;
    const filepath = path.join(HISTORY_DIR, filename);

    try {
        await fs.writeFile(filepath, JSON.stringify(detailedLog, null, 2));
        console.log(`[History] Saved firing log to ${filename}`);
    } catch (error) {
        console.error(`[History] Error saving firing log:`, error);
    } finally {
        isLogging = false;
        currentFiringSession = null;
    }
};

