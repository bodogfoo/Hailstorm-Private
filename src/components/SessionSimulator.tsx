import { useMemo, useState } from 'react';
import { MonitorPlay, PauseCircle, PlayCircle, RefreshCw, Terminal } from 'lucide-react';
import type { ExecutionState, GpuRetentionMode, SimSessionSlot } from '../types';

const INITIAL_SLOTS: SimSessionSlot[] = [
  // Group 1 (Main Tank & Core)
  { slotIndex: 1, sessionId: 'slot01', account: 'ezbox01', character: 'Vanguard', classShort: 'WAR', role: 'Tank', groupNumber: 1, zone: 'potimeb', state: 'FOREGROUND_FULL', rgMercsState: 'Running' },
  { slotIndex: 2, sessionId: 'slot02', account: 'ezbox02', character: 'Aegisheal', classShort: 'CLR', role: 'Healer', groupNumber: 1, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 3, sessionId: 'slot03', account: 'ezbox03', character: 'Cantabile', classShort: 'BRD', role: 'Support', groupNumber: 1, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 4, sessionId: 'slot04', account: 'ezbox04', character: 'Mesmerize', classShort: 'ENC', role: 'Support', groupNumber: 1, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 5, sessionId: 'slot05', account: 'ezbox05', character: 'Ironpaw', classShort: 'MNK', role: 'DPS', groupNumber: 1, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 6, sessionId: 'slot06', account: 'ezbox06', character: 'Bloodaxe', classShort: 'BER', role: 'DPS', groupNumber: 1, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  // Group 2 (Melee Off-Tank)
  { slotIndex: 7, sessionId: 'slot07', account: 'ezbox07', character: 'Dawnward', classShort: 'PAL', role: 'Tank', groupNumber: 2, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 8, sessionId: 'slot08', account: 'ezbox08', character: 'Spiritmender', classShort: 'SHM', role: 'Healer', groupNumber: 2, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 9, sessionId: 'slot09', account: 'ezbox09', character: 'Lyricist', classShort: 'BRD', role: 'Support', groupNumber: 2, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 10, sessionId: 'slot10', account: 'ezbox10', character: 'Backstab', classShort: 'ROG', role: 'DPS', groupNumber: 2, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 11, sessionId: 'slot11', account: 'ezbox11', character: 'Feralclaw', classShort: 'BST', role: 'DPS', groupNumber: 2, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 12, sessionId: 'slot12', account: 'ezbox12', character: 'Cleaver', classShort: 'BER', role: 'DPS', groupNumber: 2, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  // Group 3 (Caster Burn)
  { slotIndex: 13, sessionId: 'slot13', account: 'ezbox13', character: 'Dreadknight', classShort: 'SHD', role: 'Tank', groupNumber: 3, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 14, sessionId: 'slot14', account: 'ezbox14', character: 'Sylvanmend', classShort: 'DRU', role: 'Healer', groupNumber: 3, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 15, sessionId: 'slot15', account: 'ezbox15', character: 'Sonata', classShort: 'BRD', role: 'Support', groupNumber: 3, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 16, sessionId: 'slot16', account: 'ezbox16', character: 'Pyrebolt', classShort: 'WIZ', role: 'DPS', groupNumber: 3, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 17, sessionId: 'slot17', account: 'ezbox17', character: 'Embercore', classShort: 'MAG', role: 'DPS', groupNumber: 3, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 18, sessionId: 'slot18', account: 'ezbox18', character: 'Gravebind', classShort: 'NEC', role: 'DPS', groupNumber: 3, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  // Group 4 (Ranger/Caster Utility)
  { slotIndex: 19, sessionId: 'slot19', account: 'ezbox19', character: 'Sanctum', classShort: 'CLR', role: 'Healer', groupNumber: 4, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 20, sessionId: 'slot20', account: 'ezbox20', character: 'Hawkeye', classShort: 'RNG', role: 'DPS', groupNumber: 4, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 21, sessionId: 'slot21', account: 'ezbox21', character: 'Windarrow', classShort: 'RNG', role: 'DPS', groupNumber: 4, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 22, sessionId: 'slot22', account: 'ezbox22', character: 'Sunburst', classShort: 'WIZ', role: 'DPS', groupNumber: 4, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 23, sessionId: 'slot23', account: 'ezbox23', character: 'Cinderpet', classShort: 'MAG', role: 'DPS', groupNumber: 4, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
  { slotIndex: 24, sessionId: 'slot24', account: 'ezbox24', character: 'Chronomancer', classShort: 'ENC', role: 'Support', groupNumber: 4, zone: 'potimeb', state: 'BACKGROUND_HEADLESS', rgMercsState: 'Running' },
];

const MODE_PARAMS: Record<
  GpuRetentionMode,
  { fgVramGb: number; bgVramGb: number; fgRamGb: number; bgRamGb: number; switchMs: number; label: string }
> = {
  cached: {
    fgVramGb: 1.85,
    bgVramGb: 0.28,
    fgRamGb: 1.75,
    bgRamGb: 0.68,
    switchMs: 115,
    label: 'WarmCache (--gpu-mode cached)',
  },
  deferred: {
    fgVramGb: 1.85,
    bgVramGb: 0.08,
    fgRamGb: 1.75,
    bgRamGb: 0.62,
    switchMs: 390,
    label: 'Deferred (--gpu-mode deferred)',
  },
  'shared-broker': {
    fgVramGb: 1.55,
    bgVramGb: 0.14,
    fgRamGb: 1.65,
    bgRamGb: 0.64,
    switchMs: 155,
    label: 'Shared GPU Broker (--gpu-mode shared-broker)',
  },
};

interface SessionSimulatorProps {
  resetNonce: number;
}

export function SessionSimulator({ resetNonce }: SessionSimulatorProps) {
  const [slots, setSlots] = useState<SimSessionSlot[]>(INITIAL_SLOTS);
  const [gpuMode, setGpuMode] = useState<GpuRetentionMode>('cached');
  const [bgTickHz, setBgTickHz] = useState<number>(25);
  const [commandInput, setCommandInput] = useState<string>('/bca /rc start');
  const [lastDispatch, setLastDispatch] = useState<string>(
    'IPC bus ready. Click any slot card or execute /switch, /bca, /bcaa, or /rc.'
  );
  const [lastResetSeen, setLastResetSeen] = useState<number>(resetNonce);

  if (resetNonce !== lastResetSeen) {
    setLastResetSeen(resetNonce);
    setSlots(INITIAL_SLOTS);
    setGpuMode('cached');
    setBgTickHz(25);
    setLastDispatch('Reset simulation to 1 FOREGROUND_FULL + 23 BACKGROUND_HEADLESS at 25 Hz.');
  }

  const switchForeground = (targetSessionId: string) => {
    const target = slots.find(
      (s) =>
        s.sessionId.toLowerCase() === targetSessionId.toLowerCase() ||
        s.character.toLowerCase() === targetSessionId.toLowerCase()
    );
    if (!target) {
      setLastDispatch(`Unknown session or character "${targetSessionId}"`);
      return;
    }

    setSlots((prev) =>
      prev.map((s) => ({
        ...s,
        state: s.sessionId === target.sessionId ? 'TRANSITIONING' : 'BACKGROUND_HEADLESS',
      }))
    );

    window.setTimeout(() => {
      setSlots((prev) =>
        prev.map((s) => ({
          ...s,
          state: s.sessionId === target.sessionId ? 'FOREGROUND_FULL' : 'BACKGROUND_HEADLESS',
        }))
      );
      setLastDispatch(
        `Switched FOREGROUND_FULL -> ${target.character} (${target.sessionId}); expected switch budget ${MODE_PARAMS[gpuMode].switchMs} ms`
      );
    }, 120);
  };

  const executeCommand = (e: React.FormEvent) => {
    e.preventDefault();
    const raw = commandInput.trim();
    if (!raw) return;

    const parts = raw.split(/\s+/);
    const verb = parts[0].toLowerCase();
    const fg = slots.find((s) => s.state === 'FOREGROUND_FULL');

    if (verb === '/switch' && parts[1]) {
      switchForeground(parts[1]);
      return;
    }

    const applyAction = (targets: (s: SimSessionSlot) => boolean, payload: string, label: string) => {
      const lower = payload.toLowerCase();
      let count = 0;
      setSlots((prev) =>
        prev.map((s) => {
          if (!targets(s)) return s;
          count += 1;
          if (lower.includes('pause')) return { ...s, rgMercsState: 'Paused' };
          if (lower.includes('start') || lower.includes('unpause') || lower.includes('rgmercs')) {
            return { ...s, rgMercsState: 'Running' };
          }
          if (lower.includes('stop')) return { ...s, rgMercsState: 'Idle' };
          return s;
        })
      );
      setLastDispatch(`${label} dispatched "${payload}" -> ${count} session ACK(s)`);
    };

    if (verb === '/bca') {
      applyAction((s) => s.sessionId !== fg?.sessionId, parts.slice(1).join(' '), '/bca');
    } else if (verb === '/bcaa') {
      applyAction(() => true, parts.slice(1).join(' '), '/bcaa');
    } else if (verb === '/bcg') {
      applyAction((s) => s.groupNumber === fg?.groupNumber && s.sessionId !== fg?.sessionId, parts.slice(1).join(' '), '/bcg');
    } else if (verb === '/rc' || verb === '/bct') {
      const targetName = (parts[1] ?? '').toLowerCase();
      applyAction(
        (s) => s.character.toLowerCase() === targetName || s.sessionId.toLowerCase() === targetName,
        parts.slice(2).join(' '),
        `${verb} ${parts[1] ?? ''}`
      );
    } else {
      setLastDispatch(`Executed on foreground (${fg?.character ?? 'none'}): ${raw}`);
    }
  };

  const budget = useMemo(() => {
    const p = MODE_PARAMS[gpuMode];
    const fgCount = slots.filter((s) => s.state === 'FOREGROUND_FULL' || s.state === 'TRANSITIONING').length;
    const bgCount = slots.filter((s) => s.state === 'BACKGROUND_HEADLESS').length;
    const totalVramGb = fgCount * p.fgVramGb + bgCount * p.bgVramGb;
    const totalRamGb = fgCount * p.fgRamGb + bgCount * p.bgRamGb + 0.25; // broker + OS overhead
    return {
      fgCount,
      bgCount,
      totalVramGb,
      totalRamGb,
      switchMs: p.switchMs,
      vramPct: Math.min(100, (totalVramGb / 32) * 100),
      ramPct: Math.min(100, (totalRamGb / 48) * 100),
    };
  }, [slots, gpuMode]);

  const fgSlot = slots.find((s) => s.state === 'FOREGROUND_FULL' || s.state === 'TRANSITIONING');

  const stateBadge = (st: ExecutionState) => {
    if (st === 'FOREGROUND_FULL') {
      return 'bg-emerald-500/15 text-emerald-300 border-emerald-500/40';
    }
    if (st === 'TRANSITIONING') {
      return 'bg-amber-500/15 text-amber-300 border-amber-500/40';
    }
    if (st === 'BACKGROUND_HEADLESS') {
      return 'bg-slate-800 text-slate-300 border-slate-700';
    }
    return 'bg-red-500/15 text-red-300 border-red-500/40';
  };

  return (
    <div className="space-y-6">
      {/* Architecture Budget Overview */}
      <div className="grid grid-cols-1 gap-4 sm:grid-cols-2 lg:grid-cols-4">
        <div className="border border-slate-800 bg-[#111827] px-4 py-3.5">
          <div className="text-xs font-medium uppercase tracking-wider text-slate-400">
            Active Foreground
          </div>
          <div className="mt-1.5 flex items-baseline justify-between">
            <span className="text-xl font-semibold text-slate-100">
              {fgSlot ? fgSlot.character : 'None'}
            </span>
            <span className="font-mono text-xs tabular-nums text-emerald-400">
              {budget.fgCount} FG / {budget.bgCount} BG
            </span>
          </div>
          <div className="mt-1 text-xs text-slate-400">
            Full Vulkan + ImGui + Audio ({fgSlot?.sessionId ?? '—'})
          </div>
        </div>

        <div className="border border-slate-800 bg-[#111827] px-4 py-3.5">
          <div className="text-xs font-medium uppercase tracking-wider text-slate-400">
            Modeled System RAM (48 GiB)
          </div>
          <div className="mt-1.5 flex items-baseline justify-between">
            <span className="font-mono text-xl font-semibold tabular-nums text-slate-100">
              {budget.totalRamGb.toFixed(1)} GiB
            </span>
            <span className="font-mono text-xs tabular-nums text-slate-400">
              {budget.ramPct.toFixed(0)}% of 48 GiB
            </span>
          </div>
          <div className="mt-2 h-1.5 w-full bg-slate-800">
            <div
              className="h-full bg-teal-400 transition-all"
              style={{ width: `${budget.ramPct}%` }}
            />
          </div>
        </div>

        <div className="border border-slate-800 bg-[#111827] px-4 py-3.5">
          <div className="text-xs font-medium uppercase tracking-wider text-slate-400">
            Modeled Arc Pro B70 VRAM (32 GiB)
          </div>
          <div className="mt-1.5 flex items-baseline justify-between">
            <span className="font-mono text-xl font-semibold tabular-nums text-slate-100">
              {budget.totalVramGb.toFixed(1)} GiB
            </span>
            <span className="font-mono text-xs tabular-nums text-slate-400">
              {budget.vramPct.toFixed(0)}% of 32 GiB
            </span>
          </div>
          <div className="mt-2 h-1.5 w-full bg-slate-800">
            <div
              className="h-full bg-sky-400 transition-all"
              style={{ width: `${budget.vramPct}%` }}
            />
          </div>
        </div>

        <div className="border border-slate-800 bg-[#111827] px-4 py-3.5">
          <div className="text-xs font-medium uppercase tracking-wider text-slate-400">
            Headless Tick &amp; Switch Budget
          </div>
          <div className="mt-1.5 flex items-baseline justify-between">
            <span className="font-mono text-xl font-semibold tabular-nums text-slate-100">
              {bgTickHz} Hz
            </span>
            <span
              className={`font-mono text-xs tabular-nums ${
                budget.switchMs <= 250 ? 'text-emerald-400' : 'text-amber-400'
              }`}
            >
              ~{budget.switchMs} ms switch
            </span>
          </div>
          <div className="mt-1 text-xs text-slate-400">
            {Math.round(1000 / bgTickHz)} ms decoupled sim / nav / RG Mercs pulse
          </div>
        </div>
      </div>

      {/* Controls & IPC Bar */}
      <div className="grid grid-cols-1 gap-4 border border-slate-800 bg-[#111827] p-4 lg:grid-cols-12">
        <div className="lg:col-span-4">
          <label className="block text-xs font-medium uppercase tracking-wider text-slate-400">
            Background GPU Retention (--gpu-mode)
          </label>
          <select
            value={gpuMode}
            onChange={(e) => setGpuMode(e.target.value as GpuRetentionMode)}
            className="mt-1.5 w-full border border-slate-700 bg-[#0B1120] px-3 py-2 text-sm text-slate-100 focus:border-teal-400 focus:outline-none"
          >
            <option value="cached">cached — Keep zone buffers for &lt;250ms switches</option>
            <option value="shared-broker">shared-broker — Linux VK_KHR_external_memory_fd</option>
            <option value="deferred">deferred — Minimum VRAM footprint (~390ms switch)</option>
          </select>
        </div>

        <div className="lg:col-span-3">
          <div className="flex items-center justify-between text-xs font-medium uppercase tracking-wider text-slate-400">
            <span>Background Tick (--bg-tick-hz)</span>
            <span className="font-mono tabular-nums text-teal-300">{bgTickHz} Hz</span>
          </div>
          <input
            type="range"
            min={10}
            max={60}
            step={5}
            value={bgTickHz}
            onChange={(e) => setBgTickHz(Number(e.target.value))}
            className="mt-3 w-full accent-teal-400"
          />
        </div>

        <form onSubmit={executeCommand} className="lg:col-span-5">
          <label className="block text-xs font-medium uppercase tracking-wider text-slate-400">
            SessionBus IPC Dispatch (/switch, /bca, /bcaa, /bcg, /rc)
          </label>
          <div className="mt-1.5 flex gap-2">
            <div className="relative flex-1">
              <Terminal className="pointer-events-none absolute left-2.5 top-2.5 h-4 w-4 text-slate-500" />
              <input
                type="text"
                value={commandInput}
                onChange={(e) => setCommandInput(e.target.value)}
                placeholder="/bca /rc start or /switch Aegisheal"
                className="w-full border border-slate-700 bg-[#0B1120] py-1.5 pl-8 pr-3 font-mono text-sm text-slate-100 focus:border-teal-400 focus:outline-none"
              />
            </div>
            <button
              type="submit"
              className="whitespace-nowrap border border-teal-500/40 bg-teal-500/15 px-3.5 py-1.5 text-sm font-medium text-teal-200 hover:bg-teal-500/25"
            >
              Send IPC
            </button>
          </div>
          <div className="mt-1.5 font-mono text-xs text-slate-400">{lastDispatch}</div>
        </form>
      </div>

      {/* 4 Groups x 6 Characters Grid */}
      <div className="space-y-4">
        {([1, 2, 3, 4] as const).map((groupNum) => {
          const groupSlots = slots.filter((s) => s.groupNumber === groupNum);
          return (
            <div key={groupNum} className="border border-slate-800 bg-[#111827]">
              <div className="flex items-center justify-between border-b border-slate-800 px-4 py-2">
                <div className="flex items-center gap-2">
                  <span className="text-xs font-semibold uppercase tracking-wider text-slate-300">
                    Group {groupNum}
                  </span>
                  <span className="text-xs text-slate-500">
                    Leader: {groupSlots[0]?.character}
                  </span>
                </div>
                <span className="font-mono text-xs text-slate-400">
                  Slots {groupSlots[0]?.slotIndex}–{groupSlots[groupSlots.length - 1]?.slotIndex}
                </span>
              </div>

              <div className="grid grid-cols-1 divide-y divide-slate-800/80 md:grid-cols-2 md:divide-x md:divide-y-0 xl:grid-cols-3">
                {groupSlots.map((slot) => {
                  const isFg = slot.state === 'FOREGROUND_FULL';
                  const isTrans = slot.state === 'TRANSITIONING';
                  return (
                    <button
                      key={slot.sessionId}
                      type="button"
                      onClick={() => switchForeground(slot.sessionId)}
                      className={`flex w-full items-center justify-between px-4 py-3 text-left transition-colors hover:bg-slate-800/60 ${
                        isFg ? 'bg-emerald-950/25' : ''
                      }`}
                    >
                      <div className="min-w-0">
                        <div className="flex items-center gap-2">
                          <span className="font-mono text-xs tabular-nums text-slate-500">
                            {slot.sessionId}
                          </span>
                          <span className="truncate font-semibold text-slate-100">
                            {slot.character}
                          </span>
                          <span className="font-mono text-xs text-slate-400">
                            {slot.classShort}
                          </span>
                        </div>
                        <div className="mt-1 flex items-center gap-3 text-xs text-slate-400">
                          <span>{slot.role}</span>
                          <span className="font-mono">{slot.zone}</span>
                          <span className="inline-flex items-center gap-1">
                            {slot.rgMercsState === 'Running' ? (
                              <PlayCircle className="h-3 w-3 text-emerald-400" />
                            ) : (
                              <PauseCircle className="h-3 w-3 text-amber-400" />
                            )}
                            RG Mercs: {slot.rgMercsState}
                          </span>
                        </div>
                      </div>

                      <div className="ml-3 flex shrink-0 flex-col items-end gap-1">
                        <span
                          className={`inline-flex items-center gap-1 border px-2 py-0.5 font-mono text-[11px] font-medium ${stateBadge(
                            slot.state
                          )}`}
                        >
                          {isFg && <MonitorPlay className="h-3 w-3" />}
                          {isTrans && <RefreshCw className="h-3 w-3 animate-spin" />}
                          {slot.state}
                        </span>
                        <span className="font-mono text-[11px] tabular-nums text-slate-500">
                          {isFg
                            ? `60 FPS / ${MODE_PARAMS[gpuMode].fgVramGb.toFixed(2)} GiB`
                            : `0 FPS · ${bgTickHz} Hz / ${MODE_PARAMS[gpuMode].bgVramGb.toFixed(2)} GiB`}
                        </span>
                      </div>
                    </button>
                  );
                })}
              </div>
            </div>
          );
        })}
      </div>
    </div>
  );
}
