import React from 'react';
import { BrowserRouter as Router, Routes, Route, Link } from 'react-router-dom';
import EnhancedDashboard from './EnhancedDashboard';
import ScheduleEditor from './ScheduleEditor';

const App: React.FC = () => {
  return (
    <Router>
      <div className="min-h-screen bg-gray-900 text-white font-sans">
        <nav className="p-4 bg-gray-800 flex gap-4 border-b border-gray-700">
          <h1 className="text-xl font-bold text-orange-500">TAP II Clone</h1>
          <Link to="/" className="hover:text-orange-300">Dashboard</Link>
          <Link to="/editor" className="hover:text-orange-300">Schedule Editor</Link>
          <Link to="/settings" className="hover:text-orange-300">Settings</Link>
        </nav>

        <main className="p-4">
          <Routes>
            <Route path="/" element={<EnhancedDashboard />} />
            <Route path="/editor" element={<ScheduleEditor />} />
          </Routes>
        </main>
      </div>
    </Router>
  );
};

export default App;