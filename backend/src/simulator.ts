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
    timeRemaining: number; // Minutes remaining
    history: {x: number, y: number}[]; // x: elapsed hours, y: temp
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
    elapsedTime: 0,
    timeRemaining: 0,
    history: []
};

// SIMULATION PARAMETERS
const SIM_SPEED = 60; // 1 real second = 60 simulated seconds (1 minute)
const TICK_RATE = 1000; // ms

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
        // Update Elapsed Time (Simulated)
        // We add SIM_SPEED seconds to elapsed time every tick
        kilnState.elapsedTime += (TICK_RATE / 1000 * SIM_SPEED) / 60; // in minutes
        kilnState.timeRemaining = Math.max(0, kilnState.estimatedDuration - kilnState.elapsedTime);

        // Step Management
        if (kilnState.schedule.length > 0) {
            const currentStep = kilnState.schedule[kilnState.stepIndex];
            
            if (currentStep) {
                // RAMP
                if (currentStep.type === 'ramp') {
                    kilnState.status = 'RAMP';
                    kilnState.target = currentStep.target;
                    
                    // Calculate required change per tick based on rate
                    // Rate is deg/hr.
                    // Sim speed: 1 sec = 1 min.
                    // Rate per min = rate / 60.
                    // Change per tick = (rate / 60) * (SIM_SPEED / 60) * (TICK_RATE/1000) ???
                    // Simplification: We advanced SIM_SPEED seconds.
                    // Hours advanced = SIM_SPEED / 3600.
                    const hoursAdvanced = SIM_SPEED / 3600;
                    const rate = currentStep.rate || 100;
                    const maxChange = rate * hoursAdvanced;

                    if (kilnState.temp < kilnState.target) {
                        kilnState.temp += maxChange;
                        if (kilnState.temp > kilnState.target) kilnState.temp = kilnState.target;
                    } else if (kilnState.temp > kilnState.target) {
                        kilnState.temp -= maxChange;
                        if (kilnState.temp < kilnState.target) kilnState.temp = kilnState.target;
                    }

                    // Check completion
                    if (Math.abs(kilnState.temp - kilnState.target) < 0.5) {
                        kilnState.stepIndex++;
                        kilnState.holdStartTime = kilnState.elapsedTime; // Mark start of hold in sim time
                    }
                } 
                // HOLD
                else if (currentStep.type === 'hold') {
                    kilnState.status = 'HOLD';
                    kilnState.target = currentStep.target;
                    
                    // Maintain temp (add noise)
                    kilnState.temp = kilnState.target + (Math.random() - 0.5);

                    // Check duration
                    const duration = currentStep.holdTime || 0;
                    if ((kilnState.elapsedTime - kilnState.holdStartTime) >= duration) {
                        kilnState.stepIndex++;
                    }
                }
            } else {
                kilnState.status = 'COMPLETE';
            }
        }

        // Record History (every 5 ticks to save space? or every tick)
        // x: elapsed hours
        kilnState.history.push({
            x: kilnState.elapsedTime / 60,
            y: kilnState.temp
        });

        // Limit history size (e.g. last 1000 points)
        // if (kilnState.history.length > 1000) kilnState.history.shift();

        // Zone variance
        kilnState.t2 = kilnState.temp - 2 + (Math.random());
        kilnState.t3 = kilnState.temp + 1.5 - (Math.random());

    } else {
        // Cool to room temp
        if (kilnState.temp > 25) {
            kilnState.temp -= 0.5; // Cool faster when off
            if (kilnState.temp < 25) kilnState.temp = 25;
            kilnState.t2 = kilnState.temp;
            kilnState.t3 = kilnState.temp;
        }
        // Clear history if IDLE for a while? No, keep it until new start.
    }

    // 2. Broadcast
    const data = JSON.stringify(kilnState);
    wss.clients.forEach(client => {
        if (client.readyState === 1) { // OPEN
            client.send(data);
        }
    });

}, TICK_RATE);

export const startSimulation = (schedule: any) => {
    kilnState.schedule = schedule.steps || [];
    kilnState.stepIndex = 0;
    kilnState.status = 'RAMP';
    kilnState.energy = 0;
    kilnState.startTime = Date.now();
    kilnState.elapsedTime = 0;
    kilnState.history = [{x: 0, y: 25}];
    kilnState.temp = 25;
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
    if (kilnState.status === 'IDLE' || kilnState.status === 'COMPLETE') return;
    console.log("Skipping Step");
    if (kilnState.stepIndex < kilnState.schedule.length - 1) {
        kilnState.stepIndex++;
    } else {
        kilnState.status = 'COMPLETE';
    }
};

export const addHoldTime = (minutes: number, targetStepIndex?: number) => {
    if (kilnState.status === 'IDLE' || kilnState.status === 'COMPLETE') return;

    if (targetStepIndex !== undefined) {
        // Modify specific step
        const step = kilnState.schedule[targetStepIndex];
        if (step && step.type === 'hold') {
             step.holdTime = (step.holdTime || 0) + minutes;
             console.log(`Extended step ${targetStepIndex + 1} by ${minutes} min`);
        }
        return;
    }

    const currentStep = kilnState.schedule[kilnState.stepIndex];
    
    if (currentStep && currentStep.type === 'hold') {
        currentStep.holdTime = (currentStep.holdTime || 0) + minutes;
        console.log(`Extended current hold by ${minutes} min. New total: ${currentStep.holdTime}`);
    } else {
        // Try to find next hold step to extend
        let found = false;
        for (let i = kilnState.stepIndex + 1; i < kilnState.schedule.length; i++) {
            if (kilnState.schedule[i].type === 'hold') {
                kilnState.schedule[i].holdTime = (kilnState.schedule[i].holdTime || 0) + minutes;
                console.log(`Extended next hold (step ${i+1}) by ${minutes} min`);
                found = true;
                break;
            }
        }
        if (!found) console.log("No hold step found to extend");
    }
};

export const addTemperature = (degrees: number, targetStepIndex?: number) => {
    if (kilnState.status === 'IDLE' || kilnState.status === 'COMPLETE') return;
    
    if (targetStepIndex !== undefined) {
        const step = kilnState.schedule[targetStepIndex];
        if (step) {
            step.target += degrees;
            // If modifying current step, update global target too
            if (targetStepIndex === kilnState.stepIndex) {
                kilnState.target += degrees;
            }
            console.log(`Increased target of step ${targetStepIndex + 1} by ${degrees}C. New target: ${step.target}`);
        }
        return;
    }

    if (kilnState.schedule[kilnState.stepIndex]) {
        kilnState.schedule[kilnState.stepIndex].target += degrees;
        kilnState.target += degrees;
        console.log(`Increased target by ${degrees}C. New target: ${kilnState.target}`);
    }
};
