import express, { Router } from 'express';
import fs from 'fs';
import fsPromises from 'fs/promises';
import path from 'path';
import { Schedule, Step } from './schedule_model';

const router = Router();
const SCHEDULES_FILE = path.join(__dirname, '../../data/schedules.json');

// Ensure data directory exists
if (!fs.existsSync(path.dirname(SCHEDULES_FILE))) {
    fs.mkdirSync(path.dirname(SCHEDULES_FILE), { recursive: true });
    fs.writeFileSync(SCHEDULES_FILE, '[]');
}

// --- Helper Functions ---
const loadSchedules = (): Schedule[] => {
    try {
        const data = fs.readFileSync(SCHEDULES_FILE, 'utf-8');
        return JSON.parse(data);
    } catch (e) {
        return [];
    }
};

const saveSchedules = (schedules: Schedule[]) => {
    fs.writeFileSync(SCHEDULES_FILE, JSON.stringify(schedules, null, 2));
};

// --- Routes ---

// Get all schedules
router.get('/schedules', (req, res) => {
    const schedules = loadSchedules();
    res.json(schedules);
});

// Save a schedule
router.post('/schedules', (req, res) => {
    const newSchedule: Schedule = req.body;
    const schedules = loadSchedules();
    // Update existing or add new
    const existingIndex = schedules.findIndex(s => s.id === newSchedule.id);
    if (existingIndex >= 0) {
        schedules[existingIndex] = newSchedule;
    } else {
        newSchedule.id = Date.now().toString(); // Simple ID
        newSchedule.created = new Date();
        schedules.push(newSchedule);
    }
    saveSchedules(schedules);
    res.json(newSchedule);
});

// Delete a schedule
router.delete('/schedules/:id', (req, res) => {
    const { id } = req.params;
    let schedules = loadSchedules();
    schedules = schedules.filter(s => s.id !== id);
    saveSchedules(schedules);
    res.json({ success: true });
});

// --- Glass Wizard Generator ---
router.post('/schedules/glass', (req, res) => {
    const { coe, type, thickness } = req.body; // coe: 90|96|82(float), type: "full"|"tack"|"slump"|"polish"
    
    const steps: Step[] = [];
    
    // Base temperatures
    let processTemp = 804; // Default Full Fuse (COE 90)
    let annealTemp = 482;
    let strainPoint = 371;

    // Adjust for COE
    if (coe === 96) {
        processTemp = 796; // Slightly lower for 96
        annealTemp = 510;
        strainPoint = 395;
    } else if (coe === 82) { // Float/Bottle/Window
        processTemp = 820; // Float is stiffer
        annealTemp = 538;  // Higher anneal
        strainPoint = 470; 
    }
    
    // Adjust for Process Type
    if (type === 'tack') processTemp = (coe === 82) ? 780 : 730;
    if (type === 'slump') processTemp = (coe === 82) ? 720 : 663;
    if (type === 'polish') processTemp = (coe === 82) ? 760 : 715;
    
    // 1. Initial Heat
    const rate1 = (thickness > 6) ? 150 : 222; // Thicker glass = slower
    steps.push({ type: 'ramp', rate: rate1, target: 677 });
    steps.push({ type: 'hold', holdTime: 30 }); // Bubble squeeze
    
    // 2. Process
    steps.push({ type: 'ramp', rate: 333, target: processTemp });
    steps.push({ type: 'hold', holdTime: 10 });
    
    // 3. Crash Cool to Anneal
    steps.push({ type: 'ramp', rate: 9999, target: annealTemp });
    
    // 4. Anneal Soak
    // Float glass needs longer anneal usually
    let annealTime = (thickness > 6) ? 120 : 60;
    if (coe === 82) annealTime += 30; 

    steps.push({ type: 'hold', holdTime: annealTime });
    
    // 5. Cool to Strain Point
    steps.push({ type: 'ramp', rate: 83, target: strainPoint });
    
    // 6. Cool to Room Temp
    steps.push({ type: 'ramp', rate: 9999, target: 25 });
    
    const schedule: Schedule = {
        id: Date.now().toString(),
        name: `Glass ${coe} ${type}`,
        type: 'glass',
        steps,
        created: new Date(),
        glassDetails: { coe, type, thickness }
    };
    
    res.json(schedule);
});

// --- Settings & Live Control ---
let appSettings = {
    wattage: 3.0,
    costPerKwh: 0.15,
    currency: '$',
    zones: 1
};

router.post('/settings', (req, res) => {
    appSettings = { ...appSettings, ...req.body };
    res.json({ success: true });
});

router.get('/settings', (req, res) => {
    res.json(appSettings);
});

import { startSimulation, stopSimulation, skipStep, addHoldTime, addTemperature, startAutotune, kilnState } from './simulator';

// ... (existing imports)

// ... (existing routes)

// Get Current Status
router.get('/status', (req, res) => {
    // Calculate remaining time dynamically
    let remaining = 0;
    if (kilnState.status !== 'IDLE' && kilnState.status !== 'COMPLETE') {
        // Future steps
        for (let i = kilnState.stepIndex + 1; i < kilnState.schedule.length; i++) {
            const step = kilnState.schedule[i];
            if (step.type === 'hold') remaining += (step.holdTime || 0);
            else if (step.type === 'ramp') {
                const prev = kilnState.schedule[i-1]?.target || 25;
                const diff = Math.abs(step.target - prev);
                remaining += (diff / (step.rate || 100)) * 60;
            }
        }
        // Current step (add full duration for simplicity since we don't track elapsed in step precisely in sim)
        const current = kilnState.schedule[kilnState.stepIndex];
        if (current) {
            if (current.type === 'hold') remaining += (current.holdTime || 0);
            else remaining += 15; // Approx for ramp
        }
    }

    res.json({
        ...kilnState,
        step: kilnState.stepIndex + 1,
        totalSteps: kilnState.schedule.length,
        timeRemaining: Math.floor(remaining)
    });
});

// Start Firing (Updated to use simulator)
router.post('/start', (req, res) => {
    // In a real app, validation logic here
    const schedule = req.body; 
    startSimulation(schedule); // Start the sim
    res.json({ status: 'started' });
});

// Autotune
router.post('/autotune', (req, res) => {
    const { temp } = req.body;
    startAutotune(temp || 150);
    res.json({ status: 'tuning_started' });
});

// Stop Firing
router.post('/stop', (req, res) => {
    stopSimulation();
    res.json({ status: 'stopped' });
});

// Live Control Routes
router.post('/skip', (req, res) => {
    skipStep();
    res.json({ success: true });
});

router.post('/addTime', (req, res) => {
    const { minutes, stepIndex } = req.body;
    addHoldTime(Number(minutes) || 5, stepIndex !== undefined ? Number(stepIndex) : undefined);
    res.json({ success: true });
});

router.post('/addTemp', (req, res) => {
    const { degrees, stepIndex } = req.body;
    addTemperature(Number(degrees) || 5, stepIndex !== undefined ? Number(stepIndex) : undefined);
    res.json({ success: true });
});

router.get('/diagnostics', (req, res) => {
    // Mock Diagnostics
    res.json({
        relays: { zone1: true, safety: true },
        elements: { resistance: 12.5, health: 98 },
        thermocouple: { temp: kilnState.temp, status: 'OK' },
        ground: 'OK'
    });
});

// --- History API ---
const HISTORY_DIR = path.join(__dirname, '../../data/history');

// GET /api/history - List all firing sessions
router.get('/history', async (req, res) => {
    try {
        await fsPromises.mkdir(HISTORY_DIR, { recursive: true });
        const files = await fsPromises.readdir(HISTORY_DIR);
        const summaries = await Promise.all(
            files
                .filter(f => f.endsWith('.json'))
                .map(async (file) => {
                    const content = await fsPromises.readFile(path.join(HISTORY_DIR, file), 'utf-8');
                    const log = JSON.parse(content);
                    return log.summary; // Return only the summary part
                })
        );
        // Sort by start time, newest first
        summaries.sort((a, b) => (new Date(b.startTime).getTime()) - (new Date(a.startTime).getTime()));
        res.json(summaries);
    } catch (error) {
        // If directory doesn't exist (e.g. permissions issue), return empty array gracefully.
        if (error && typeof error === 'object' && 'code' in error && (error as { code: string }).code === 'ENOENT') {
            return res.json([]);
        }
        console.error('Error reading history directory:', error);
        res.status(500).json({ error: 'Failed to fetch history' });
    }
});

// GET /api/history/:id - Get details for a specific session
router.get('/history/:id', async (req, res) => {
    const { id } = req.params;
    // Basic sanitization to prevent directory traversal
    if (!id.match(/^[a-zA-Z0-9_\-]+$/)) {
        return res.status(400).json({ error: 'Invalid history ID format' });
    }

    const filepath = path.join(HISTORY_DIR, `${id}.json`);

    try {
        const content = await fsPromises.readFile(filepath, 'utf-8');
        const log = JSON.parse(content);
        res.json(log);
    } catch (error) {
        if (error && typeof error === 'object' && 'code' in error && (error as { code: string }).code === 'ENOENT') {
            return res.status(404).json({ error: 'History log not found' });
        }
        console.error(`Error reading history file ${id}.json:`, error);
        res.status(500).json({ error: 'Failed to fetch history detail' });
    }
});

export const apiRouter = router;
