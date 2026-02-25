import express from 'express';
import http from 'http';
import { WebSocketServer } from 'ws';
import cors from 'cors';
import { apiRouter } from './enhanced_api';
import './simulator'; // Start the simulator loop

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });

app.use(cors());
app.use(express.json());

// Mount API routes
app.use('/api', apiRouter);

// WebSocket handling for real-time updates
wss.on('connection', (ws) => {
    console.log('Client connected');
    // Send initial state immediately
    import('./simulator').then(sim => {
        ws.send(JSON.stringify(sim.kilnState));
    });
});

const PORT = process.env.PORT || 3001;
server.listen(PORT, () => {
    console.log(`Kiln Controller Backend running on port ${PORT}`);
});

export { wss };
