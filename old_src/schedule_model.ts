export type StepType = 'RAMP' | 'HOLD' | 'COOL';

export interface ScheduleStep {
    id: string;
    type: StepType;
    targetTemp: number; // Celsius
    duration?: number;  // Minutes (for HOLD)
    rate?: number;      // Degrees per hour (for RAMP)
}

export interface FiringSchedule {
    id: string;
    name: string;
    steps: ScheduleStep[];
    createdAt: Date;
}

export interface KilnStatus {
    currentTemp: number;
    targetTemp: number;
    state: 'IDLE' | 'FIRING' | 'COMPLETE' | 'ERROR';
    currentStepIndex: number;
}