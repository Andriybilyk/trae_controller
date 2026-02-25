import express, { Router } from 'express';
import fs from 'fs';
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

import { startSimulation, stopSimulation, skipStep, addHold, startAutotune, kilnState } from './simulator';

// ... (existing imports)

// ... (existing routes)

// Get Current Status
router.get('/status', (req, res) => {
    res.json(kilnState);
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

// Mock control endpoints (Updated)
router.post('/control/start', (req, res) => {
    const { name } = req.body;
    // Just a quick manual start for demo
    startSimulation({ steps: [{type:'ramp', target: 500}, {type:'hold', target: 500}] });
    res.json({ success: true });
});

router.post('/control/stop', (req, res) => {
    stopSimulation();
    res.json({ success: true });
});

router.post('/control/skip', (req, res) => {
    console.log("Skipping Step");
    skipStep();
    res.json({ success: true });
});

router.post('/control/hold', (req, res) => {
    const { minutes } = req.body;
    console.log(`Adding ${minutes} min hold`);
    addHold(minutes);
    res.json({ success: true });
});

export const apiRouter = router;
