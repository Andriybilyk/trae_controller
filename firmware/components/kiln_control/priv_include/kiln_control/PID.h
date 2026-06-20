#ifndef PID_H
#define PID_H

#define AUTOMATIC 1
#define MANUAL    0
#define DIRECT    0
#define REVERSE   1

class PID {
public:
    PID(double* Input, double* Output, double* Setpoint,
        double Kp, double Ki, double Kd, int ControllerDirection) {
        myInput = Input;
        myOutput = Output;
        mySetpoint = Setpoint;
        inAuto = false;
        SetOutputLimits(0, 255);
        SampleTime = 100; // 100ms
        SetControllerDirection(ControllerDirection);
        SetTunings(Kp, Ki, Kd);
        lastTime = 0;
    }

    void SetMode(int Mode) {
        bool newAuto = (Mode == AUTOMATIC);
        if (newAuto && !inAuto) {
            Initialize();
        }
        inAuto = newAuto;
    }

    bool Compute() {
        if (!inAuto) return false;
        double input = *myInput;
        double error = *mySetpoint - input;
        double dInput = (input - lastInput);
        const double proposedITerm = ITerm + (ki * error);
        double unclamped = kp * error + proposedITerm - kd * dInput;

        // Conditional integration prevents the I term from winding up while the output is saturated.
        const bool saturatingHigh = unclamped > outMax;
        const bool saturatingLow = unclamped < outMin;
        if (!((saturatingHigh && error > 0) || (saturatingLow && error < 0))) {
            ITerm = proposedITerm;
            if (ITerm > outMax) ITerm = outMax;
            else if (ITerm < outMin) ITerm = outMin;
            unclamped = kp * error + ITerm - kd * dInput;
        }

        double output = unclamped;
        if (output > outMax) output = outMax;
        else if (output < outMin) output = outMin;
        *myOutput = output;

        lastInput = input;
        return true;
    }

    void SetOutputLimits(double Min, double Max) {
        if (Min >= Max) return;
        outMin = Min;
        outMax = Max;
        if (inAuto) {
            if (*myOutput > outMax) *myOutput = outMax;
            else if (*myOutput < outMin) *myOutput = outMin;
            if (ITerm > outMax) ITerm = outMax;
            else if (ITerm < outMin) ITerm = outMin;
        }
    }

    void SetTunings(double Kp, double Ki, double Kd) {
        if (Kp < 0 || Ki < 0 || Kd < 0) return;
        dispKp = Kp; dispKi = Ki; dispKd = Kd;
        double SampleTimeInSec = ((double)SampleTime) / 1000;
        kp = Kp;
        ki = Ki * SampleTimeInSec;
        kd = Kd / SampleTimeInSec;
        if (controllerDirection == REVERSE) {
            kp = (0 - kp);
            ki = (0 - ki);
            kd = (0 - kd);
        }
    }

    void SetControllerDirection(int Direction) {
        if (inAuto && Direction != controllerDirection) {
            kp = (0 - kp);
            ki = (0 - ki);
            kd = (0 - kd);
        }
        controllerDirection = Direction;
    }

    void SetSampleTime(int NewSampleTime) {
        if (NewSampleTime > 0) {
            double ratio = (double)NewSampleTime / (double)SampleTime;
            ki *= ratio;
            kd /= ratio;
            SampleTime = (unsigned long)NewSampleTime;
        }
    }

private:
    void Initialize() {
        ITerm = *myOutput;
        lastInput = *myInput;
        if (ITerm > outMax) ITerm = outMax;
        else if (ITerm < outMin) ITerm = outMin;
    }

    double dispKp, dispKi, dispKd;
    double kp, ki, kd;
    int controllerDirection;
    double *myInput, *myOutput, *mySetpoint;
    unsigned long lastTime, SampleTime;
    double ITerm, lastInput;
    double outMin, outMax;
    bool inAuto;
};

#endif
