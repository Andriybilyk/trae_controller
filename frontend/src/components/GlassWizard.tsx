import React, { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Beaker, ArrowRight } from 'lucide-react';
import { useLanguage } from '../contexts/LanguageContext';
import { API_BASE_URL } from '../config';

const GlassWizard = () => {
  const { t } = useLanguage();
  const [coe, setCoe] = useState(90);
  const [type, setType] = useState('full');
  const [thickness, setThickness] = useState(6);
  const [loading, setLoading] = useState(false);
  const navigate = useNavigate();

  const handleGenerate = async () => {
    setLoading(true);
    try {
      const response = await fetch(`${API_BASE_URL}/schedules/glass`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ coe, type, thickness }),
      });
      const schedule = await response.json();
      await fetch(`${API_BASE_URL}/schedules`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(schedule),
      });
      navigate('/schedules');
    } catch (error) {
      console.error('Error generating schedule:', error);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="flex flex-col gap-6 max-w-4xl mx-auto">
      <div className="flex items-center gap-4 mb-2">
        <div className="p-3 bg-cyan-500/10 rounded-xl border border-cyan-500/20">
            <Beaker size={32} className="text-cyan-400" />
        </div>
        <div>
            <h1 className="text-3xl font-bold text-white tracking-tight">{t.wizard.title}</h1>
            <p className="text-slate-400 text-sm">{t.wizard.subtitle}</p>
        </div>
      </div>

      <div className="bg-kiln-card border border-kiln-border rounded-xl p-6 shadow-lg">
        <div className="mb-8">
            <label className="text-xs text-slate-500 font-bold uppercase mb-4 block">{t.wizard.coe}</label>
            <div className="grid grid-cols-1 md:grid-cols-3 gap-4">
                 <button 
                    className={`p-6 rounded-xl border-2 transition-all text-center flex flex-col items-center justify-center gap-2 ${coe === 90 ? 'bg-cyan-500/10 border-cyan-500 text-white shadow-lg shadow-cyan-500/20' : 'bg-zinc-800 border-zinc-700 text-zinc-400 hover:bg-zinc-700 hover:border-zinc-600'}`}
                    onClick={() => setCoe(90)}
                >
                    <div className="text-2xl font-bold">{t.wizard.coe90}</div>
                    <div className="text-xs font-bold uppercase tracking-wider opacity-70">{t.wizard.bullseye}</div>
                </button>
                <button 
                    className={`p-6 rounded-xl border-2 transition-all text-center flex flex-col items-center justify-center gap-2 ${coe === 96 ? 'bg-cyan-500/10 border-cyan-500 text-white shadow-lg shadow-cyan-500/20' : 'bg-zinc-800 border-zinc-700 text-zinc-400 hover:bg-zinc-700 hover:border-zinc-600'}`}
                    onClick={() => setCoe(96)}
                >
                    <div className="text-2xl font-bold">{t.wizard.coe96}</div>
                    <div className="text-xs font-bold uppercase tracking-wider opacity-70">{t.wizard.system96}</div>
                </button>
                <button 
                    className={`p-6 rounded-xl border-2 transition-all text-center flex flex-col items-center justify-center gap-2 ${coe === 82 ? 'bg-cyan-500/10 border-cyan-500 text-white shadow-lg shadow-cyan-500/20' : 'bg-zinc-800 border-zinc-700 text-zinc-400 hover:bg-zinc-700 hover:border-zinc-600'}`}
                    onClick={() => setCoe(82)}
                >
                    <div className="text-2xl font-bold">{t.wizard.float}</div>
                    <div className="text-xs font-bold uppercase tracking-wider opacity-70">{t.wizard.windowGlass}</div>
                </button>
            </div>
        </div>

        <div className="mb-8">
            <label className="text-xs text-slate-500 font-bold uppercase mb-4 block">{t.wizard.type}</label>
            <select 
                className="w-full bg-zinc-900 border-zinc-700 focus:border-cyan-500 p-4 text-lg rounded-xl border text-white"
                value={type} 
                onChange={(e) => setType(e.target.value)}
            >
                <option value="full">{t.wizard.fullFuse}</option>
                <option value="tack">{t.wizard.tackFuse}</option>
                <option value="slump">{t.wizard.slump}</option>
                <option value="polish">{t.wizard.polish}</option>
            </select>
        </div>

        <div className="mb-8">
            <label className="text-xs text-slate-500 font-bold uppercase mb-4 block">{t.wizard.thickness}</label>
            <div className="bg-zinc-900/50 p-6 rounded-xl border border-zinc-700">
                <input 
                    type="range" 
                    min="1" 
                    max="20" 
                    value={thickness} 
                    onChange={(e) => setThickness(Number(e.target.value))} 
                    className="w-full mb-6 accent-cyan-500 h-2 bg-zinc-700 rounded-lg appearance-none cursor-pointer"
                />
                <div className="flex justify-between text-sm font-medium text-slate-500 font-mono">
                    <span>1 mm</span>
                    <span className="text-cyan-400 font-bold text-xl">{thickness} mm</span>
                    <span>20 mm</span>
                </div>
            </div>
        </div>

        <button 
            className="w-full py-4 bg-cyan-600 hover:bg-cyan-500 text-white rounded-xl text-lg font-bold shadow-lg shadow-cyan-500/20 flex items-center justify-center gap-2 transition-all"
            onClick={handleGenerate} 
            disabled={loading}
        >
            {loading ? t.wizard.calculating : (
                <>{t.wizard.generate} <ArrowRight size={20} /></>
            )}
        </button>
      </div>
    </div>
  );
};

export default GlassWizard;
