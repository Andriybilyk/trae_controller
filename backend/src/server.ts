import express from 'express';
import http from 'http';
import { WebSocketServer, WebSocket } from 'ws';
import cors from 'cors';
import { apiRouter } from './enhanced_api';
import * as logger from './history_logger';

// --- CONFIGURATION ---
const DEVICE_IP = process.env.DEVICE_IP || 'kiln.local';
const DEVICE_WS_URL = `ws://${DEVICE_IP}/ws`;

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });

app.use(cors());
app.use(express.json());

// --- Firing State & History ---
// Intercept start/stop calls to manage logging
app.post('/api/start', (req, res, next) => {
    const schedule = req.body;
    const totalSteps = schedule?.steps?.length || 0;
    logger.startLogging(schedule?.name || 'Unknown Schedule', totalSteps);
    next(); // Continue to the actual API handler
});

app.post('/api/stop', (req, res, next) => {
    logger.stopLogging('STOPPED');
    next();
});

// Mount the actual API router
app.use('/api', apiRouter);

// --- WebSocket Proxy & Broadcasting ---

// Function to connect to the ESP32 WebSocket
const connectToDevice = () => {
    console.log(`Connecting to device at ${DEVICE_WS_URL}...`);
    const deviceWs = new WebSocket(DEVICE_WS_URL);

    deviceWs.on('open', () => {
        console.log('Successfully connected to ESP32 WebSocket.');
    });

    deviceWs.on('message', (data) => {
        const message = data.toString();
        // 1. Log the data point
        try {
            const parsedData = JSON.parse(message);
            logger.logDataPoint(parsedData);

            // Check for firing completion or error
            if (parsedData.status === 'COMPLETE') {
                logger.stopLogging('COMPLETED');
            } else if (parsedData.status === 'ERROR') {
                logger.stopLogging('ERROR');
            }

        } catch (e) {
            // Not a json message, probably a simple connect message
        }

        // 2. Broadcast to all connected browser clients
        wss.clients.forEach((client) => {
            if (client.readyState === WebSocket.OPEN) {
                client.send(message);
            }
        });
    });

    deviceWs.on('close', () => {
        console.log('Device WebSocket disconnected. Retrying in 5 seconds...');
        setTimeout(connectToDevice, 5000);
    });

    deviceWs.on('error', (err) => {
        console.error('Error with device WebSocket connection:', err.message);
        // The 'close' event will fire next, triggering a reconnect attempt.
    });
};

// Start the connection process
connectToDevice();


// Handle connections from browser clients
wss.on('connection', (ws) => {
    console.log('Browser client connected');
    ws.on('close', () => {
        console.log('Browser client disconnected');
    });
});

const PORT = process.env.PORT || 3001;
server.listen(PORT, () => {
    console.log(`Kiln Controller Backend running on port ${PORT}`);
});

export { wss };
