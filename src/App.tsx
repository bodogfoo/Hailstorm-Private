import { useEffect, useState } from 'react';
import {
  Cpu,
  Download,
  FileText,
  KeyRound,
  Layers,
  RotateCcw,
  ShieldCheck,
} from 'lucide-react';
import { CompatibilityMatrix } from './components/CompatibilityMatrix';
import { DocsAndPatchView } from './components/DocsAndPatchView';
import { MqLoginBuilder } from './components/MqLoginBuilder';
import { SessionSimulator } from './components/SessionSimulator';
import type { OverviewResponse } from './types';

type TabId = 'simulator' | 'mq-import' | 'matrix' | 'docs';

const NAV_ITEMS: Array<{ id: TabId; label: string; sub: string; icon: typeof Cpu }> = [
  { id: 'simulator', label: '24-Box Simulator', sub: 'ExecutionState & VRAM budget', icon: Cpu },
  { id: 'mq-import', label: 'MQ Login Importer', sub: 'Keyring & roster CLI builder', icon: KeyRound },
  { id: 'matrix', label: 'RG Mercs Matrix', sub: 'TLO, Lua & IPC coverage', icon: Layers },
  { id: 'docs', label: 'Patch & Handoff', sub: 'Ubuntu build & deliverables', icon: FileText },
];

export default function App() {
  const [activeTab, setActiveTab] = useState<TabId>('simulator');
  const [overview, setOverview] = useState<OverviewResponse | null>(null);
  const [resetNonce, setResetNonce] = useState(0);

  useEffect(() => {
    fetch('/api/overview')
      .then((res) => (res.ok ? (res.json() as Promise<OverviewResponse>) : null))
      .then((json) => {
        if (json) setOverview(json);
      })
      .catch(() => {});
  }, []);

  const currentNav = NAV_ITEMS.find((n) => n.id === activeTab) ?? NAV_ITEMS[0];

  return (
    <div className="flex min-h-screen bg-[#0B1120] text-slate-100">
      {/* Sidebar Navigation */}
      <aside className="hidden w-64 shrink-0 flex-col border-r border-slate-800 bg-[#0F172A] md:flex">
        <div className="border-b border-slate-800 px-5 py-4">
          <div className="flex items-center gap-2">
            <ShieldCheck className="h-5 w-5 text-teal-400" />
            <span className="text-base font-bold tracking-tight text-slate-100">
              Hailstone 24
            </span>
          </div>
          <div className="mt-1 font-mono text-[11px] text-slate-400">
            RoF2 / EZ Server · GPL-2.0
          </div>
        </div>

        <nav className="flex-1 space-y-1 p-3">
          {NAV_ITEMS.map((item) => {
            const Icon = item.icon;
            const active = activeTab === item.id;
            return (
              <button
                key={item.id}
                type="button"
                onClick={() => setActiveTab(item.id)}
                className={`flex w-full items-start gap-3 border px-3 py-2.5 text-left transition-colors ${
                  active
                    ? 'border-teal-500/40 bg-teal-500/10 text-teal-200'
                    : 'border-transparent text-slate-400 hover:bg-slate-800/60 hover:text-slate-200'
                }`}
              >
                <Icon className={`mt-0.5 h-4 w-4 shrink-0 ${active ? 'text-teal-400' : 'text-slate-500'}`} />
                <div>
                  <div className="text-sm font-medium">{item.label}</div>
                  <div className="text-xs text-slate-500">{item.sub}</div>
                </div>
              </button>
            );
          })}
        </nav>

        <div className="border-t border-slate-800 p-4 text-xs text-slate-400">
          <div className="font-semibold uppercase tracking-wider text-slate-300">
            Target Hardware
          </div>
          <div className="mt-2 space-y-1 font-mono tabular-nums">
            <div className="flex justify-between">
              <span className="text-slate-500">OS</span>
              <span>{overview?.workstation.os ?? 'Ubuntu Linux'}</span>
            </div>
            <div className="flex justify-between">
              <span className="text-slate-500">GPU</span>
              <span>{overview?.workstation.gpu ?? 'Arc Pro B70'}</span>
            </div>
            <div className="flex justify-between">
              <span className="text-slate-500">VRAM</span>
              <span>{overview?.workstation.vramGb ?? 32} GiB</span>
            </div>
            <div className="flex justify-between">
              <span className="text-slate-500">RAM</span>
              <span>{overview?.workstation.ramGb ?? 48} GiB</span>
            </div>
          </div>
        </div>
      </aside>

      {/* Main Content */}
      <div className="flex min-w-0 flex-1 flex-col">
        {/* Top Bar Contract: Breadcrumb left, max 2 primary actions right */}
        <header className="flex flex-wrap items-center justify-between gap-3 border-b border-slate-800 bg-[#0F172A] px-6 py-3.5">
          <div>
            <div className="flex items-center gap-2 text-xs text-slate-400">
              <span>Project Hailstone 24</span>
              <span>/</span>
              <span className="text-slate-200">{currentNav.label}</span>
            </div>
            <h1 className="mt-0.5 text-lg font-semibold text-slate-100">{currentNav.sub}</h1>
          </div>

          <div className="flex items-center gap-2.5">
            {activeTab === 'simulator' && (
              <button
                type="button"
                onClick={() => setResetNonce((n) => n + 1)}
                className="inline-flex shrink-0 items-center gap-1.5 whitespace-nowrap border border-slate-700 bg-[#111827] px-3 py-1.5 text-xs font-medium text-slate-200 hover:bg-slate-800"
              >
                <RotateCcw className="h-3.5 w-3.5" />
                Reset Simulation
              </button>
            )}
            <a
              href="/api/download/patch"
              className="inline-flex shrink-0 items-center gap-1.5 whitespace-nowrap border border-teal-500/40 bg-teal-500/15 px-3 py-1.5 text-xs font-medium text-teal-200 hover:bg-teal-500/25"
            >
              <Download className="h-3.5 w-3.5" />
              Download hailstone24.patch
            </a>
          </div>
        </header>

        {/* Mobile Nav Bar */}
        <div className="flex overflow-x-auto border-b border-slate-800 bg-[#111827] px-3 py-2 md:hidden">
          {NAV_ITEMS.map((item) => (
            <button
              key={item.id}
              type="button"
              onClick={() => setActiveTab(item.id)}
              className={`shrink-0 whitespace-nowrap px-3 py-1.5 text-xs font-medium ${
                activeTab === item.id ? 'bg-teal-500/15 text-teal-200' : 'text-slate-400'
              }`}
            >
              {item.label}
            </button>
          ))}
        </div>

        <main className="mx-auto w-full max-w-[1440px] flex-1 p-6">
          {activeTab === 'simulator' && <SessionSimulator resetNonce={resetNonce} />}
          {activeTab === 'mq-import' && <MqLoginBuilder />}
          {activeTab === 'matrix' && <CompatibilityMatrix />}
          {activeTab === 'docs' && <DocsAndPatchView overview={overview} />}
        </main>
      </div>
    </div>
  );
}
