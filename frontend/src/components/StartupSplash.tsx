import React from 'react';
import { Flame } from 'lucide-react';

export const StartupSplash: React.FC = () => {
  return (
    <div className="fixed inset-0 z-[400] flex items-center justify-center bg-[#05070b]">
      <div className="relative flex flex-col items-center gap-6">
        <div className="absolute -inset-16 bg-emerald-400/10 blur-3xl rounded-full" />
        <div className="relative w-36 h-36 rounded-3xl bg-gradient-to-br from-emerald-300 to-emerald-500 shadow-[0_0_60px_rgba(16,185,129,0.4)] flex items-center justify-center">
          <Flame size={86} className="text-black fill-black drop-shadow-[0_4px_12px_rgba(0,0,0,0.3)]" />
        </div>
        <div className="relative text-6xl md:text-7xl font-extrabold text-white tracking-tight">KilnPro</div>
      </div>
    </div>
  );
};

