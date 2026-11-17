import { useMemo, useState } from 'react';
import { Check, Copy, Download, KeyRound, ShieldAlert } from 'lucide-react';
import type { GpuRetentionMode } from '../types';

type SourceKind = 'login.db' | 'MQ2AutoLogin.ini' | 'delimited';

export function MqLoginBuilder() {
  const [sourceKind, setSourceKind] = useState<SourceKind>('login.db');
  const [mqSourcePath, setMqSourcePath] = useState('/mnt/win/MacroQuest/config/login.db');
  const [dataDir, setDataDir] = useState('~/.local/share/hailstone24');
  const [eqGameDir, setEqGameDir] = useState('/opt/eq/rof2');
  const [serverName, setServerName] = useState('EZ Server');
  const [serverHost, setServerHost] = useState('eqemulator.net');
  const [serverPort, setServerPort] = useState(5999);
  const [useMasterEnv, setUseMasterEnv] = useState(true);
  const [gpuMode, setGpuMode] = useState<GpuRetentionMode>('cached');
  const [bgTickHz, setBgTickHz] = useState(25);
  const [copiedId, setCopiedId] = useState<string | null>(null);

  const serverId = useMemo(
    () =>
      serverName
        .toLowerCase()
        .replace(/[^a-z0-9]+/g, '-')
        .replace(/^-|-$/g, '') || 'ez-server',
    [serverName]
  );

  const importCmd = useMemo(() => {
    const lines = [];
    if (useMasterEnv && sourceKind === 'login.db') {
      lines.push('read -s -p "MQ Master Password (Enter if none): " MQ_MASTER_PASSWORD && echo');
      lines.push('export MQ_MASTER_PASSWORD');
    }
    lines.push('./build/hailstone/hailstone \\');
    lines.push(`  --data-dir "${dataDir}" \\`);
    lines.push(`  --game-dir "${eqGameDir}" \\`);
    lines.push(`  --import-mq-logins "${mqSourcePath}" \\`);
    lines.push(`  --server "${serverId}" --host "${serverHost}" --port ${serverPort} \\`);
    lines.push(`  --roster-out "${dataDir}/hailstone24_roster.ini"`);
    if (useMasterEnv && sourceKind === 'login.db') {
      lines.push('unset MQ_MASTER_PASSWORD');
    }
    return lines.join('\n');
  }, [useMasterEnv, sourceKind, dataDir, eqGameDir, mqSourcePath, serverId, serverHost, serverPort]);

  const launchCmd = useMemo(() => {
    return [
      `# Terminal 1 — Broker + 24 Staggered Auto-Login Sessions (reads passwords solely from libsecret keyring)`,
      `./build/hailstone/hailstone24_broker \\`,
      `  --data-dir "${dataDir}" \\`,
      `  --game-dir "${eqGameDir}" \\`,
      `  --roster "${dataDir}/hailstone24_roster.ini" \\`,
      `  --launch-all \\`,
      `  --bg-tick-hz ${bgTickHz} \\`,
      `  --gpu-mode ${gpuMode} \\`,
      `  --stagger-ms 400`,
      ``,
      `# Terminal 2 — Control Deck (SDL3 Desktop UI, or pass --tui)`,
      `./build/hailstone/hailstone24_manager`,
    ].join('\n');
  }, [dataDir, eqGameDir, bgTickHz, gpuMode]);

  const rosterPreview = useMemo(() => {
    const header = [
      `# Generated passwordless Hailstone 24 roster (never contains credentials)`,
      `[global]`,
      `default_server=${serverId}`,
      `default_world=${serverName}`,
      `bg_tick_hz=${bgTickHz}`,
      `gpu_mode=${gpuMode}`,
      `auto_kick=1`,
      ``,
    ];
    const slots = Array.from({ length: 24 }, (_, idx) => {
      const n = String(idx + 1).padStart(2, '0');
      const headless = idx === 0 ? 0 : 1;
      const leader = `BoxChar${String(Math.floor(idx / 6) * 6 + 1).padStart(2, '0')}`;
      return [
        `[slot${n}]`,
        `enabled=1`,
        `session_id=slot${n}`,
        `server_id=${serverId}`,
        `world=${serverName}`,
        `account=mq_acct_${n}`,
        `character=BoxChar${n}`,
        `group_leader=${leader}`,
        `headless=${headless}`,
      ].join('\n');
    });
    return [...header, ...slots].join('\n\n');
  }, [serverId, serverName, bgTickHz, gpuMode]);

  const copyBlock = async (id: string, text: string) => {
    await navigator.clipboard.writeText(text);
    setCopiedId(id);
    window.setTimeout(() => setCopiedId((prev) => (prev === id ? null : prev)), 1500);
  };

  const downloadRoster = () => {
    const blob = new Blob([rosterPreview], { type: 'text/plain;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'hailstone24_roster.ini';
    a.click();
    URL.revokeObjectURL(url);
  };

  return (
    <div className="space-y-6">
      <div className="flex items-start gap-3 border border-teal-500/30 bg-teal-950/20 px-4 py-3 text-sm text-slate-200">
        <KeyRound className="mt-0.5 h-4 w-4 shrink-0 text-teal-400" />
        <div>
          <span className="font-semibold text-teal-200">Read-Only MQ Import &amp; Keyring Isolation: </span>
          <span className="text-slate-300">
            <code className="font-mono text-xs text-teal-300">--import-mq-logins</code> opens{' '}
            <code className="font-mono text-xs">login.db</code> with{' '}
            <code className="font-mono text-xs">SQLITE_OPEN_READONLY</code>, decrypts XOR entries in memory, stores secrets solely in Ubuntu{' '}
            <code className="font-mono text-xs">libsecret</code>, wipes buffers via{' '}
            <code className="font-mono text-xs">core::wipe()</code>, and never modifies your existing EverQuest or MacroQuest folders.
          </span>
        </div>
      </div>

      <div className="grid grid-cols-1 gap-6 lg:grid-cols-12">
        {/* Form Inputs */}
        <div className="space-y-4 border border-slate-800 bg-[#111827] p-5 lg:col-span-5">
          <h2 className="text-sm font-semibold uppercase tracking-wider text-slate-200">
            MacroQuest Source &amp; Isolation Parameters
          </h2>

          <div>
            <label className="block text-xs font-medium text-slate-400">MQ AutoLogin Source Format</label>
            <div className="mt-1.5 grid grid-cols-3 gap-2">
              {(['login.db', 'MQ2AutoLogin.ini', 'delimited'] as const).map((kind) => (
                <button
                  key={kind}
                  type="button"
                  onClick={() => {
                    setSourceKind(kind);
                    if (kind === 'login.db') setMqSourcePath('/mnt/win/MacroQuest/config/login.db');
                    if (kind === 'MQ2AutoLogin.ini') setMqSourcePath('/mnt/win/MacroQuest/config/MQ2AutoLogin.ini');
                    if (kind === 'delimited') setMqSourcePath('/mnt/win/MacroQuest/config/exported_logins.txt');
                  }}
                  className={`border px-2.5 py-1.5 font-mono text-xs transition-colors ${
                    sourceKind === kind
                      ? 'border-teal-400 bg-teal-500/15 text-teal-200'
                      : 'border-slate-700 bg-[#0B1120] text-slate-400 hover:text-slate-200'
                  }`}
                >
                  {kind}
                </button>
              ))}
            </div>
          </div>

          <div>
            <label className="block text-xs font-medium text-slate-400">
              Source File Path (opened read-only)
            </label>
            <input
              type="text"
              value={mqSourcePath}
              onChange={(e) => setMqSourcePath(e.target.value)}
              className="mt-1 w-full border border-slate-700 bg-[#0B1120] px-3 py-1.5 font-mono text-xs text-slate-100 focus:border-teal-400 focus:outline-none"
            />
          </div>

          <div>
            <label className="block text-xs font-medium text-slate-400">
              Isolated Hailstone Data Folder (--data-dir)
            </label>
            <input
              type="text"
              value={dataDir}
              onChange={(e) => setDataDir(e.target.value)}
              className="mt-1 w-full border border-slate-700 bg-[#0B1120] px-3 py-1.5 font-mono text-xs text-slate-100 focus:border-teal-400 focus:outline-none"
            />
          </div>

          <div>
            <label className="block text-xs font-medium text-slate-400">
              EverQuest RoF2 Data Directory (--game-dir, untouched)
            </label>
            <input
              type="text"
              value={eqGameDir}
              onChange={(e) => setEqGameDir(e.target.value)}
              className="mt-1 w-full border border-slate-700 bg-[#0B1120] px-3 py-1.5 font-mono text-xs text-slate-100 focus:border-teal-400 focus:outline-none"
            />
          </div>

          <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
            <div>
              <label className="block text-xs font-medium text-slate-400">Target Server</label>
              <input
                type="text"
                value={serverName}
                onChange={(e) => setServerName(e.target.value)}
                className="mt-1 w-full border border-slate-700 bg-[#0B1120] px-2.5 py-1.5 text-xs text-slate-100 focus:border-teal-400 focus:outline-none"
              />
            </div>
            <div>
              <label className="block text-xs font-medium text-slate-400">Login Host</label>
              <input
                type="text"
                value={serverHost}
                onChange={(e) => setServerHost(e.target.value)}
                className="mt-1 w-full border border-slate-700 bg-[#0B1120] px-2.5 py-1.5 font-mono text-xs text-slate-100 focus:border-teal-400 focus:outline-none"
              />
            </div>
            <div>
              <label className="block text-xs font-medium text-slate-400">Port</label>
              <input
                type="number"
                value={serverPort}
                onChange={(e) => setServerPort(Number(e.target.value) || 5999)}
                className="mt-1 w-full border border-slate-700 bg-[#0B1120] px-2.5 py-1.5 font-mono text-xs tabular-nums text-slate-100 focus:border-teal-400 focus:outline-none"
              />
            </div>
          </div>

          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block text-xs font-medium text-slate-400">GPU Mode</label>
              <select
                value={gpuMode}
                onChange={(e) => setGpuMode(e.target.value as GpuRetentionMode)}
                className="mt-1 w-full border border-slate-700 bg-[#0B1120] px-2.5 py-1.5 font-mono text-xs text-slate-100 focus:border-teal-400 focus:outline-none"
              >
                <option value="cached">cached</option>
                <option value="shared-broker">shared-broker</option>
                <option value="deferred">deferred</option>
              </select>
            </div>
            <div>
              <label className="block text-xs font-medium text-slate-400">Background Hz</label>
              <input
                type="number"
                min={10}
                max={60}
                value={bgTickHz}
                onChange={(e) => setBgTickHz(Math.min(60, Math.max(10, Number(e.target.value) || 25)))}
                className="mt-1 w-full border border-slate-700 bg-[#0B1120] px-2.5 py-1.5 font-mono text-xs tabular-nums text-slate-100 focus:border-teal-400 focus:outline-none"
              />
            </div>
          </div>

          {sourceKind === 'login.db' && (
            <label className="flex items-center gap-2.5 pt-1 text-xs text-slate-300">
              <input
                type="checkbox"
                checked={useMasterEnv}
                onChange={(e) => setUseMasterEnv(e.target.checked)}
                className="accent-teal-400"
              />
              Prompt via <code className="font-mono text-teal-300">$MQ_MASTER_PASSWORD</code> (recommended over CLI flags)
            </label>
          )}

          <div className="flex items-start gap-2 border border-amber-500/25 bg-amber-950/15 px-3 py-2 text-xs text-amber-200/90">
            <ShieldAlert className="mt-0.5 h-3.5 w-3.5 shrink-0 text-amber-400" />
            <span>
              Windows DPAPI blobs cannot be decrypted on Linux; plaintext and MacroQuest XOR master-password entries import automatically.
            </span>
          </div>
        </div>

        {/* Generated Commands & Passwordless Roster */}
        <div className="space-y-4 lg:col-span-7">
          <div className="border border-slate-800 bg-[#111827]">
            <div className="flex items-center justify-between border-b border-slate-800 px-4 py-2.5">
              <span className="text-xs font-semibold uppercase tracking-wider text-slate-300">
                Step 1 — Import Accounts &amp; Passwords Into Keyring
              </span>
              <button
                type="button"
                onClick={() => copyBlock('import', importCmd)}
                className="inline-flex items-center gap-1.5 border border-slate-700 px-2.5 py-1 text-xs text-slate-300 hover:bg-slate-800"
              >
                {copiedId === 'import' ? <Check className="h-3.5 w-3.5 text-emerald-400" /> : <Copy className="h-3.5 w-3.5" />}
                {copiedId === 'import' ? 'Copied' : 'Copy'}
              </button>
            </div>
            <pre className="overflow-x-auto p-4 font-mono text-xs leading-relaxed text-slate-200">
              {importCmd}
            </pre>
          </div>

          <div className="border border-slate-800 bg-[#111827]">
            <div className="flex items-center justify-between border-b border-slate-800 px-4 py-2.5">
              <span className="text-xs font-semibold uppercase tracking-wider text-slate-300">
                Step 2 — Launch Broker &amp; 24 Isolated Sessions
              </span>
              <button
                type="button"
                onClick={() => copyBlock('launch', launchCmd)}
                className="inline-flex items-center gap-1.5 border border-slate-700 px-2.5 py-1 text-xs text-slate-300 hover:bg-slate-800"
              >
                {copiedId === 'launch' ? <Check className="h-3.5 w-3.5 text-emerald-400" /> : <Copy className="h-3.5 w-3.5" />}
                {copiedId === 'launch' ? 'Copied' : 'Copy'}
              </button>
            </div>
            <pre className="overflow-x-auto p-4 font-mono text-xs leading-relaxed text-slate-200">
              {launchCmd}
            </pre>
          </div>

          <div className="border border-slate-800 bg-[#111827]">
            <div className="flex items-center justify-between border-b border-slate-800 px-4 py-2.5">
              <span className="text-xs font-semibold uppercase tracking-wider text-slate-300">
                Passwordless Roster Preview (hailstone24_roster.ini)
              </span>
              <button
                type="button"
                onClick={downloadRoster}
                className="inline-flex items-center gap-1.5 border border-teal-500/40 bg-teal-500/15 px-2.5 py-1 text-xs font-medium text-teal-200 hover:bg-teal-500/25"
              >
                <Download className="h-3.5 w-3.5" />
                Download Template INI
              </button>
            </div>
            <pre className="max-h-56 overflow-y-auto p-4 font-mono text-xs leading-relaxed text-slate-400">
              {rosterPreview}
            </pre>
          </div>
        </div>
      </div>
    </div>
  );
}
