import React, { useState } from 'react';
import { ScheduleStep } from '../backend/schedule_model';

const ScheduleEditor: React.FC = () => {
  const [steps, setSteps] = useState<ScheduleStep[]>([]);
  const [name, setName] = useState('');

  const addStep = (type: 'RAMP' | 'HOLD') => {
    const newStep: ScheduleStep = {
      id: Date.now().toString(),
      type,
      targetTemp: 0,
      duration: type === 'HOLD' ? 10 : undefined,
      rate: type === 'RAMP' ? 100 : undefined
    };
    setSteps([...steps, newStep]);
  };

  const updateStep = (index: number, field: keyof ScheduleStep, value: number) => {
    const newSteps = [...steps];
    // @ts-ignore
    newSteps[index][field] = value;
    setSteps(newSteps);
  };

  const saveSchedule = async () => {
    // API call to save
    console.log('Saving:', { name, steps });
    alert('Schedule Saved!');
  };

  return (
    <div className="bg-gray-800 p-6 rounded-lg max-w-4xl mx-auto">
      <h2 className="text-2xl font-bold mb-6">Create Firing Schedule</h2>
      
      <div className="mb-6">
        <label className="block text-gray-400 mb-2">Schedule Name</label>
        <input 
          type="text" 
          value={name}
          onChange={(e) => setName(e.target.value)}
          className="w-full bg-gray-700 p-2 rounded text-white border border-gray-600 focus:border-orange-500 outline-none"
          placeholder="e.g., Bisque Firing Cone 04"
        />
      </div>

      <div className="space-y-4 mb-8">
        {steps.map((step, idx) => (
          <div key={step.id} className="bg-gray-700 p-4 rounded flex items-center gap-4">
            <span className="font-bold text-orange-400 w-16">{step.type}</span>
            
            <div className="flex-1 grid grid-cols-2 gap-4">
              <div>
                <label className="text-xs text-gray-400">Target Temp (°C)</label>
                <input type="number" value={step.targetTemp} onChange={(e) => updateStep(idx, 'targetTemp', +e.target.value)} className="w-full bg-gray-600 p-1 rounded" />
              </div>
              {step.type === 'HOLD' && (
                <div>
                  <label className="text-xs text-gray-400">Duration (min)</label>
                  <input type="number" value={step.duration} onChange={(e) => updateStep(idx, 'duration', +e.target.value)} className="w-full bg-gray-600 p-1 rounded" />
                </div>
              )}
            </div>
            
            <button onClick={() => setSteps(steps.filter((_, i) => i !== idx))} className="text-red-400 hover:text-red-300">✕</button>
          </div>
        ))}
      </div>

      <div className="flex gap-4 border-t border-gray-700 pt-4">
        <button onClick={() => addStep('RAMP')} className="bg-blue-600 px-4 py-2 rounded hover:bg-blue-700">+ Add Ramp</button>
        <button onClick={() => addStep('HOLD')} className="bg-purple-600 px-4 py-2 rounded hover:bg-purple-700">+ Add Hold</button>
        <button onClick={saveSchedule} className="ml-auto bg-green-600 px-6 py-2 rounded font-bold hover:bg-green-700">Save Schedule</button>
      </div>
    </div>
  );
};

export default ScheduleEditor;