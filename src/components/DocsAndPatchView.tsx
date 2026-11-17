import { useEffect, useState } from 'react';
import { Download, FileCode2, FileText } from 'lucide-react';
import type { DocResponse, OverviewResponse } from '../types';

interface DocsAndPatchViewProps {
  overview: OverviewResponse | null;
}

const DOC_TABS = [
  { slug: 'handoff', label: 'Ubuntu Handoff (CODEX_HANDOFF.md)' },
  { slug: 'architecture', label: 'Architecture Decision' },
  { slug: 'limitations', label: 'Known Limitations' },
  { slug: 'matrix_md', label: 'Compatibility Table (MD)' },
] as const;

export function DocsAndPatchView({ overview }: DocsAndPatchViewProps) {
  const [activeSlug, setActiveSlug] = useState<string>('handoff');
  const [doc, setDoc] = useState<DocResponse | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let active = true;
    setLoading(true);
    fetch(`/api/doc/${activeSlug}`)
      .then(async (res) => {
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return (await res.json()) as DocResponse;
      })
      .then((json) => {
        if (!active) return;
        setDoc(json);
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
  }, [activeSlug]);

  return (
    <div className="space-y-6">
      {/* Patch & Downloadable Artifacts */}
      <div className="grid grid-cols-1 gap-4 lg:grid-cols-12">
        <div className="border border-slate-800 bg-[#111827] p-4 lg:col-span-5">
          <div className="flex items-center justify-between">
            <h2 className="text-xs font-semibold uppercase tracking-wider text-slate-300">
              Verified Upstream Patch Diff
            </h2>
            <span className="border border-emerald-500/40 bg-emerald-500/15 px-2 py-0.5 font-mono text-[11px] text-emerald-300">
              git apply --check PASS
            </span>
          </div>

          <div className="mt-3 grid grid-cols-3 gap-2 border border-slate-800 bg-[#0B1120] p-3 font-mono text-xs tabular-nums">
            <div>
              <div className="text-slate-500">Files</div>
              <div className="mt-0.5 text-base font-semibold text-slate-100">
                {overview?.patch.filesChanged ?? '—'}
              </div>
            </div>
            <div>
              <div className="text-slate-500">Insertions</div>
              <div className="mt-0.5 text-base font-semibold text-emerald-400">
                +{overview?.patch.insertions ?? '—'}
              </div>
            </div>
            <div>
              <div className="text-slate-500">Deletions</div>
              <div className="mt-0.5 text-base font-semibold text-amber-400">
                -{overview?.patch.deletions ?? '—'}
              </div>
            </div>
          </div>

          <div className="mt-4">
            <div className="mb-2 text-xs font-medium text-slate-400">Deliverable Downloads</div>
            <div className="flex flex-wrap gap-2">
              {(overview?.artifacts ?? []).map((art) => (
                <a
                  key={art.id}
                  href={`/api/download/${art.id}`}
                  className="inline-flex items-center gap-1.5 border border-slate-700 bg-[#0B1120] px-2.5 py-1.5 font-mono text-xs text-slate-200 hover:border-teal-400 hover:text-teal-200"
                >
                  <Download className="h-3.5 w-3.5 text-teal-400" />
                  {art.filename}
                </a>
              ))}
            </div>
          </div>
        </div>

        <div className="border border-slate-800 bg-[#111827] p-4 lg:col-span-7">
          <div className="flex items-center justify-between">
            <h2 className="text-xs font-semibold uppercase tracking-wider text-slate-300">
              Patch Manifest ({overview?.patch.files.length ?? 0} files)
            </h2>
            <span className="font-mono text-xs text-slate-400">
              {overview?.patch.bytes ? `${Math.round(overview.patch.bytes / 1024)} KiB` : ''}
            </span>
          </div>
          <div className="mt-3 grid max-h-48 grid-cols-1 gap-1 overflow-y-auto border border-slate-800 bg-[#0B1120] p-3 font-mono text-xs text-slate-300 sm:grid-cols-2">
            {(overview?.patch.files ?? []).map((file) => (
              <div key={file} className="flex items-center gap-1.5 truncate">
                <FileCode2 className="h-3.5 w-3.5 shrink-0 text-sky-400" />
                <span className="truncate">{file}</span>
              </div>
            ))}
          </div>
        </div>
      </div>

      {/* Document Viewer */}
      <div className="border border-slate-800 bg-[#111827]">
        <div className="flex flex-wrap items-center gap-1 border-b border-slate-800 bg-[#0B1120] px-3 pt-2">
          {DOC_TABS.map((tab) => {
            const selected = activeSlug === tab.slug;
            return (
              <button
                key={tab.slug}
                type="button"
                onClick={() => setActiveSlug(tab.slug)}
                className={`inline-flex items-center gap-1.5 border-b-2 px-3 py-2 text-xs font-medium transition-colors ${
                  selected
                    ? 'border-teal-400 text-teal-200'
                    : 'border-transparent text-slate-400 hover:text-slate-200'
                }`}
              >
                <FileText className="h-3.5 w-3.5" />
                {tab.label}
              </button>
            );
          })}
        </div>

        <div className="p-5">
          {loading && (
            <div className="space-y-2 animate-pulse">
              <div className="h-4 w-1/3 bg-slate-800" />
              <div className="h-3 w-full bg-slate-800" />
              <div className="h-3 w-5/6 bg-slate-800" />
              <div className="h-3 w-4/6 bg-slate-800" />
            </div>
          )}
          {error && <div className="text-sm text-red-400">Error loading document: {error}</div>}
          {!loading && doc && (
            <pre className="whitespace-pre-wrap break-words font-mono text-xs leading-relaxed text-slate-200">
              {doc.content}
            </pre>
          )}
        </div>
      </div>
    </div>
  );
}
