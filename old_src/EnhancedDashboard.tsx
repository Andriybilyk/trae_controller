import React, { useEffect, useState } from 'react';
import { LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer } from 'recharts';

const EnhancedDashboard: React.FC = () => {
  const [tempData, setTempData] = useState<{time: string, temp: number, target: number}[]>([]);
  const [status, setStatus] = useState({ temp: 0, state: 'IDLE', target: 0 });

  useEffect(() => {
    // Connect to WebSocket
    const ws = new WebSocket('ws://localhost:3000');
    
    ws.onmessage = (event) => {
      const data = JSON.parse(event.data);
      if (data.temp) {
        setStatus(prev => ({ ...prev, temp: data.temp }));
        setTempData(prev => [
          ...prev.slice(-20), // Keep last 20 points
          { time: new Date().toLocaleTimeString(), temp: data.temp, target: status.target }
        ]);
      }
    };

    return () => ws.close();
  }, []);

  return (
    <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
      {/* Status Card */}
      <div className="bg-gray-800 p-6 rounded-lg shadow-lg">
        <h2 className="text-2xl font-bold mb-4">Current Status</h2>
        <div className="flex justify-between items-center">
          <div>
            <p className="text-gray-400">Temperature</p>
            <p className="text-6xl font-mono text-orange-500">{status.temp}°C</p>
          </div>
          <div className="text-right">
            <p className="text-gray-400">State</p>
            <span className={`px-3 py-1 rounded ${status.state === 'FIRING' ? 'bg-red-600' : 'bg-green-600'}`}>
              {status.state}
            </span>
          </div>
        </div>
        <div className="mt-6 flex gap-4">
            <button className="bg-red-600 hover:bg-red-700 px-6 py-2 rounded font-bold w-full">ABORT</button>
        </div>
      </div>

      {/* Graph */}
      <div className="bg-gray-800 p-6 rounded-lg shadow-lg h-80">
        <h2 className="text-xl font-bold mb-2">Firing Curve</h2>
        <ResponsiveContainer width="100%" height="100%">
          <LineChart data={tempData}>
            <CartesianGrid strokeDasharray="3 3" stroke="#444" />
            <XAxis dataKey="time" stroke="#888" />
            <YAxis stroke="#888" />
            <Tooltip contentStyle={{ backgroundColor: '#333', border: 'none' }} />
            <Line type="monotone" dataKey="temp" stroke="#f97316" strokeWidth={2} dot={false} />
            <Line type="monotone" dataKey="target" stroke="#4ade80" strokeDasharray="5 5" dot={false} />
          </LineChart>
        </ResponsiveContainer>
      </div>
    </div>
  );
};

export default EnhancedDashboard;