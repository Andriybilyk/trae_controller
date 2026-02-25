export interface Step {
    type: 'ramp' | 'hold' | 'cool';
    rate?: number; // deg/hr, undefined/0 = ASAP
    target?: number; // Celsius
    holdTime?: number; // minutes
}

export interface Schedule {
    id: string;
    name: string;
    type: 'custom' | 'cone' | 'glass';
    steps: Step[];
    created: Date;
    cone?: string; // e.g. "04"
    speed?: 'slow' | 'medium' | 'fast';
    glassDetails?: {
        coe: 90 | 96;
        type: 'full' | 'tack' | 'slump';
        thickness: number; // mm
    };
}
