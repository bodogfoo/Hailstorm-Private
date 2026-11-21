import { useEffect, useMemo, useState } from 'react';
import { AlertCircle, CheckCircle2, Search, XCircle } from 'lucide-react';
import type { MatrixEntry, MatrixResponse } from '../types';

export function CompatibilityMatrix() {
  const [data, setData] = useState<MatrixResponse | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [query, setQuery] = useState('');
  const [categoryFilter, setCategoryFilter] = useState<string>('All');
  const [statusFilter, setStatusFilter] = useState<string>('All');

  useEffect(() => {
    let active = true;
    setLoading(true);
    fetch('/api/matrix')
      .then(async (res) => {
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return (await res.json()) as MatrixResponse;
      })
      .then((json) => {
        if (!active) return;
        setData(json);
        setError(null);
      })
      .catch((err) => {
        if (!active) return;
        setError((err as Error).message);
      })
      .finally(() => {
        if (active) setLoading(false);
      });
    return () => {
      active = false;
    };
  }, []);

  const categories = useMemo(() => {
    if (!data) return ['All'];
    return ['All', ...Array.from(new Set(data.entries.map((e) => e.category)))];
  }, [data]);

  const filtered = useMemo(() => {
    if (!data) return [];
    const q = query.trim().toLowerCase();
    return data.entries.filter((entry) => {
      if (categoryFilter !== 'All' && entry.category !== categoryFilter) return false;
      if (statusFilter !== 'All' && entry.status !== statusFilter) return false;
      if (!q) return true;
      return (
        entry.symbol.toLowerCase().includes(q) ||
        entry.requirement.toLowerCase().includes(q) ||
        entry.implementation.toLowerCase().includes(q) ||
        entry.notes.toLowerCase().includes(q)
      );
    });
  }, [data, query, categoryFilter, statusFilter]);

  const statusBadge = (status: MatrixEntry['status']) => {
    if (status === 'Supported') {
      return (
        <span className="inline-flex items-center gap-1 border border-emerald-500/40 bg-emerald-500/15 px-2 py-0.5 text-xs font-medium text-emerald-300">
          <CheckCircle2 className="h-3.5 w-3.5" />
          Supported
        </span>
      );
    }
    if (status === 'Partial') {
      return (
        <span className="inline-flex items-center gap-1 border border-amber-500/40 bg-amber-500/15 px-2 py-0.5 text-xs font-medium text-amber-300">
          <AlertCircle className="h-3.5 w-3.5" />
          Partial
        </span>
      );
    }
    return (
      <span className="inline-flex items-center gap-1 border border-red-500/40 bg-red-500/15 px-2 py-0.5 text-xs font-medium text-red-300">
        <XCircle className="h-3.5 w-3.5" />
        Unsupported
      </span>
    );
  };

  return (
    <div className="space-y-4">
      <div className="flex flex-col gap-3 border border-slate-800 bg-[#111827] p-4 md:flex-row md:items-center md:justify-between">
        <div className="relative flex-1">
          <Search className="pointer-events-none absolute left-3 top-2.5 h-4 w-4 text-slate-500" />
          <input
            type="text"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder="Filter by TLO, Lua module, slash command, or C++ file..."
            className="w-full border border-slate-700 bg-[#0B1120] py-1.5 pl-9 pr-3 text-sm text-slate-100 focus:border-teal-400 focus:outline-none"
          />
        </div>

        <div className="flex flex-wrap gap-2">
          <select
            value={categoryFilter}
            onChange={(e) => setCategoryFilter(e.target.value)}
            className="border border-slate-700 bg-[#0B1120] px-3 py-1.5 text-xs text-slate-200 focus:border-teal-400 focus:outline-none"
          >
            {categories.map((cat) => (
              <option key={cat} value={cat}>
                Category: {cat}
              </option>
            ))}
          </select>

          <select
            value={statusFilter}
            onChange={(e) => setStatusFilter(e.target.value)}
            className="border border-slate-700 bg-[#0B1120] px-3 py-1.5 text-xs text-slate-200 focus:border-teal-400 focus:outline-none"
          >
            <option value="All">Status: All</option>
            <option value="Supported">Supported</option>
            <option value="Partial">Partial</option>
            <option value="Unsupported">Unsupported</option>
          </select>
        </div>
      </div>

      {error && (
        <div className="border border-red-500/40 bg-red-950/20 px-4 py-3 text-sm text-red-300">
          Failed to load compatibility matrix: {error}
        </div>
      )}

      <div className="overflow-x-auto border border-slate-800 bg-[#111827]">
        <table className="w-full border-collapse text-left text-sm">
          <thead>
            <tr className="border-b border-slate-800 bg-[#0B1120] text-xs font-semibold uppercase tracking-wider text-slate-400">
              <th className="px-4 py-2.5">Category</th>
              <th className="px-4 py-2.5">API / Feature</th>
              <th className="px-4 py-2.5">Implementation</th>
              <th className="px-4 py-2.5">Status</th>
              <th className="px-4 py-2.5">Notes</th>
            </tr>
          </thead>
          <tbody className="divide-y divide-slate-800/80">
            {loading &&
              Array.from({ length: 6 }).map((_, i) => (
                <tr key={i} className="h-10 animate-pulse">
                  <td className="px-4 py-2.5">
                    <div className="h-3.5 w-20 bg-slate-800" />
                  </td>
                  <td className="px-4 py-2.5">
                    <div className="h-3.5 w-48 bg-slate-800" />
                  </td>
                  <td className="px-4 py-2.5">
                    <div className="h-3.5 w-32 bg-slate-800" />
                  </td>
                  <td className="px-4 py-2.5">
                    <div className="h-3.5 w-20 bg-slate-800" />
                  </td>
                  <td className="px-4 py-2.5">
                    <div className="h-3.5 w-56 bg-slate-800" />
                  </td>
                </tr>
              ))}

            {!loading && filtered.length === 0 && (
              <tr>
                <td colSpan={5} className="px-4 py-8 text-center text-sm text-slate-400">
                  No RG Mercs entries match the current filter.{' '}
                  <button
                    type="button"
                    onClick={() => {
                      setQuery('');
                      setCategoryFilter('All');
                      setStatusFilter('All');
                    }}
                    className="ml-1 text-teal-400 underline hover:text-teal-300"
                  >
                    Clear filters
                  </button>
                </td>
              </tr>
            )}

            {!loading &&
              filtered.map((row) => (
                <tr key={row.id} className="hover:bg-slate-800/40">
                  <td className="whitespace-nowrap px-4 py-2.5 text-xs font-medium text-slate-400">
                    {row.category}
                  </td>
                  <td className="px-4 py-2.5">
                    <div className="font-mono text-xs font-medium text-slate-100">{row.symbol}</div>
                    <div className="mt-0.5 text-xs text-slate-400">{row.requirement}</div>
                  </td>
                  <td className="whitespace-nowrap px-4 py-2.5 font-mono text-xs text-sky-300">
                    {row.implementation}
                  </td>
                  <td className="whitespace-nowrap px-4 py-2.5">{statusBadge(row.status)}</td>
                  <td className="px-4 py-2.5 text-xs text-slate-300">{row.notes}</td>
                </tr>
              ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}
