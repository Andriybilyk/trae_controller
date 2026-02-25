import { Router } from 'express';
import { FiringSchedule } from './schedule_model';
import { wss } from './server';

export const apiRouter = Router();

// Mock Database
let schedules: FiringSchedule[] = [];
let currentStatus = { temp: 25, state: 'IDLE' };

// --- Status Endpoints ---
apiRouter.get('/status', (req, res) => {
    res.json(currentStatus);
});

// --- Control Endpoints ---
apiRouter.post('/control/start', (req, res) => {
    const { scheduleId } = req.body;
    // Logic to send command to ESP32 via serial or HTTP request to IP
    console.log(`Starting schedule ${scheduleId}`);
    currentStatus.state = 'FIRING';
    
    // Broadcast update
    wss.clients.forEach(client => client.send(JSON.stringify({ type: 'STATUS', payload: currentStatus })));
    
    res.json({ success: true });
});

apiRouter.post('/control/stop', (req, res) => {
    currentStatus.state = 'IDLE';
    // Logic to stop ESP32
    res.json({ success: true });
});

// --- Schedule Management ---
apiRouter.get('/schedules', (req, res) => {
    res.json(schedules);
});

apiRouter.post('/schedules', (req, res) => {
    const newSchedule: FiringSchedule = { ...req.body, id: Date.now().toString() };
    schedules.push(newSchedule);
    res.json(newSchedule);
});