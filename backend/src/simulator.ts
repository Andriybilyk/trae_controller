import { wss } from './server';

interface KilnState {
    temp: number;
    t2: number;
    t3: number;
    target: number;
    status: 'IDLE' | 'RAMP' | 'HOLD' | 'COMPLETE' | 'ERROR' | 'TUNING';
    energy: number;
    stepIndex: number;
    schedule: any[];
    startTime: number;
    holdStartTime: number;
    estimatedDuration: number; // Total estimated minutes
    elapsedTime: number; // Minutes elapsed in firing
}

export const kilnState: KilnState = {
    temp: 25,
    t2: 25,
    t3: 25,
    target: 0,
    status: 'IDLE',
    energy: 0,
    stepIndex: 0,
    schedule: [],
    startTime: 0,
    holdStartTime: 0,
    estimatedDuration: 0,
    elapsedTime: 0
};

// Helper to calculate total duration
const calculateDuration = (schedule: any[]) => {
    let totalMinutes = 0;
    let currentTemp = 25;
    
    schedule.forEach(step => {
        if (step.type === 'ramp') {
            const diff = Math.abs((step.target || 0) - currentTemp);
            const rate = step.rate || 100; // deg/hr
            const minutes = (diff / rate) * 60;
            totalMinutes += minutes;
            currentTemp = Number(step.target);
        } else {
            totalMinutes += Number(step.holdTime || 0);
        }
    });
    return totalMinutes;
};

// Autotune Simulation Variables
let tuningPhase = 0; // 0: heat to target, 1: cool below, 2: heat above, 3: done

// Simulation Loop
setInterval(() => {
    // 1. Logic
    if (kilnState.status === 'TUNING') {
        // ... (autotune logic remains same)
        // Autotune Logic (Oscillate around target)
        if (tuningPhase === 0) {
            // Heat to target
            if (kilnState.temp < kilnState.target) {
                kilnState.temp += 2.5; // Fast heat
            } else {
                tuningPhase = 1; // Overshot, start cooling
            }
        } else if (tuningPhase === 1) {
            // Cool below target
            if (kilnState.temp > kilnState.target - 10) {
                kilnState.temp -= 1.5; // Cool
            } else {
                tuningPhase = 2; // Undershot, start heating
            }
        } else if (tuningPhase === 2) {
            // Heat above target again
            if (kilnState.temp < kilnState.target + 5) {
                kilnState.temp += 2.0;
            } else {
                tuningPhase = 3; // Done
            }
        } else {
            // Complete
            kilnState.status = 'IDLE';
            console.log("Autotune Complete");
        }
        
        // Add some noise
        kilnState.temp += (Math.random() - 0.5) * 0.5;
        kilnState.t2 = kilnState.temp - 1;
        kilnState.t3 = kilnState.temp + 1;

    } else if (kilnState.status !== 'IDLE' && kilnState.status !== 'COMPLETE' && kilnState.status !== 'ERROR') {
        // Update Elapsed Time
        if (kilnState.startTime > 0) {
            kilnState.elapsedTime = (Date.now() - kilnState.startTime) / 60000; // Minutes
        }

        // Simulate Heating
        const rampRate = 100; // degrees per tick (fast simulation)
        
        // Move temp towards target
        if (kilnState.temp < kilnState.target) {
            kilnState.temp += (Math.random() * 2) + 0.5; // Random heat
            kilnState.energy += 0.01; // Simulate energy usage
        } else if (kilnState.temp > kilnState.target) {
            kilnState.temp -= 0.5; // Cool down
        }

        // Zone variance simulation
        kilnState.t2 = kilnState.temp - 2 + (Math.random());
        kilnState.t3 = kilnState.temp + 1.5 - (Math.random());

        // Step Management (Simplified)
        if (kilnState.schedule.length > 0) {
            const currentStep = kilnState.schedule[kilnState.stepIndex];
            
            if (currentStep) {
                // RAMP
                if (currentStep.type === 'ramp') {
                    kilnState.status = 'RAMP';
                    kilnState.target = currentStep.target;
                    // If reached target, move to next or hold
                    if (Math.abs(kilnState.temp - kilnState.target) < 5) {
                        kilnState.stepIndex++;
                    }
                } 
                // HOLD
                else if (currentStep.type === 'hold') {
                    kilnState.status = 'HOLD';
                    kilnState.target = currentStep.target;
                    // In real sim, track time. Here just wait a bit randomly or assume hold logic handled elsewhere
                    // For demo, we just drift through holds slowly
                    if (Math.random() > 0.95) kilnState.stepIndex++; 
                }
            } else {
                kilnState.status = 'COMPLETE';
            }
        }
    } else {
        // Cool to room temp
        if (kilnState.temp > 25) {
            kilnState.temp -= 0.2;
            kilnState.t2 = kilnState.temp;
            kilnState.t3 = kilnState.temp;
        }
    }

    // 2. Broadcast
    const data = JSON.stringify(kilnState);
    wss.clients.forEach(client => {
        if (client.readyState === 1) { // OPEN
            client.send(data);
        }
    });

}, 1000);

export const startSimulation = (schedule: any) => {
    kilnState.schedule = schedule.steps || [];
    kilnState.stepIndex = 0;
    kilnState.status = 'RAMP';
    kilnState.energy = 0;
    kilnState.startTime = Date.now();
    kilnState.elapsedTime = 0;
    kilnState.estimatedDuration = calculateDuration(kilnState.schedule);
};

export const startAutotune = (targetTemp: number) => {
    kilnState.status = 'TUNING';
    kilnState.target = targetTemp;
    tuningPhase = 0;
    console.log(`Starting Autotune to ${targetTemp}C`);
};

export const stopSimulation = () => {
    kilnState.status = 'IDLE';
    kilnState.target = 0;
};

export const skipStep = () => {
    if (kilnState.schedule[kilnState.stepIndex + 1]) {
        kilnState.stepIndex++;
    } else {
        kilnState.status = 'COMPLETE';
    }
};

export const addHold = (minutes: number) => {
    // In a real hold, extend the duration.
    // For simulation, we can't easily "extend" without tracking time properly.
    // We'll just log it for now as "action received"
    console.log("Simulating Hold Extension");
};
