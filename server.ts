import express from 'express';
import fs from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const HAILSTONE_DIR = path.resolve(__dirname, 'hailstone24');
const DOCS_DIR = path.join(HAILSTONE_DIR, 'docs');
const PATCH_PATH = path.join(HAILSTONE_DIR, 'hailstone24.patch');

interface DocSpec {
  path: string;
  title: string;
  filename: string;
}

const DOCS: Record<string, DocSpec> = {
  handoff: {
    path: path.resolve(__dirname, 'CODEX_HANDOFF.md'),
    title: 'Ubuntu Workstation Handoff',
    filename: 'CODEX_HANDOFF.md',
  },
  architecture: {
    path: path.join(DOCS_DIR, 'ARCHITECTURE_DECISION.md'),
    title: 'Architecture Decision Record',
    filename: 'ARCHITECTURE_DECISION.md',
  },
  limitations: {
    path: path.join(DOCS_DIR, 'KNOWN_LIMITATIONS.md'),
    title: 'Known Limitations & Verification Scope',
    filename: 'KNOWN_LIMITATIONS.md',
  },
  matrix_md: {
    path: path.join(DOCS_DIR, 'RGMERCS_COMPATIBILITY_MATRIX.md'),
    title: 'RG Mercs Compatibility Matrix',
    filename: 'RGMERCS_COMPATIBILITY_MATRIX.md',
  },
};

const DOWNLOADS: Record<string, { path: string; filename: string; contentType: string }> = {
  patch: {
    path: PATCH_PATH,
    filename: 'hailstone24.patch',
    contentType: 'text/x-diff; charset=utf-8',
  },
  ini: {
    path: path.join(HAILSTONE_DIR, 'configs', 'hailstone24.ini'),
    filename: 'hailstone24.ini',
    contentType: 'text/plain; charset=utf-8',
  },
  roster: {
    path: path.join(HAILSTONE_DIR, 'configs', 'hailstone24_roster.ini'),
    filename: 'hailstone24_roster.ini',
    contentType: 'text/plain; charset=utf-8',
  },
  bench: {
    path: path.join(HAILSTONE_DIR, 'scripts', 'bench_24box.sh'),
    filename: 'bench_24box.sh',
    contentType: 'application/x-sh; charset=utf-8',
  },
  handoff: {
    path: path.resolve(__dirname, 'CODEX_HANDOFF.md'),
    filename: 'CODEX_HANDOFF.md',
    contentType: 'text/markdown; charset=utf-8',
  },
};

async function computePatchStats() {
  try {
    const raw = await fs.readFile(PATCH_PATH, 'utf8');
    const stat = await fs.stat(PATCH_PATH);
    let filesChanged = 0;
    let insertions = 0;
    let deletions = 0;
    const files: string[] = [];

    for (const line of raw.split('\n')) {
      if (line.startsWith('diff --git ')) {
        filesChanged += 1;
        const match = /^diff --git a\/(.+?) b\/(.+)$/.exec(line);
        if (match) files.push(match[2]);
      } else if (line.startsWith('+') && !line.startsWith('+++ ')) {
        insertions += 1;
      } else if (line.startsWith('-') && !line.startsWith('--- ')) {
        deletions += 1;
      }
    }

    return {
      exists: true,
      bytes: stat.size,
      filesChanged,
      insertions,
      deletions,
      files,
    };
  } catch {
    return {
      exists: false,
      bytes: 0,
      filesChanged: 0,
      insertions: 0,
      deletions: 0,
      files: [] as string[],
    };
  }
}

async function startServer() {
  const app = express();
  app.use(express.json());

  app.get('/api/overview', async (_req, res) => {
    try {
      const patch = await computePatchStats();
      const matrixRaw = await fs.readFile(path.join(DOCS_DIR, 'RGMERCS_COMPATIBILITY_MATRIX.json'), 'utf8');
      const matrix = JSON.parse(matrixRaw) as { entries: Array<{ status: string }> };

      const counts = {
        total: matrix.entries.length,
        supported: matrix.entries.filter((e) => e.status === 'Supported').length,
        partial: matrix.entries.filter((e) => e.status === 'Partial').length,
        unsupported: matrix.entries.filter((e) => e.status === 'Unsupported').length,
      };

      res.json({
        project: 'Project Hailstone 24',
        targetServer: 'EverQuest EZ Server (RoF2)',
        workstation: {
          os: 'Ubuntu Linux',
          gpu: 'Intel Arc Pro B70',
          vramGb: 32,
          ramGb: 48,
        },
        patch,
        compatibilityCounts: counts,
        artifacts: Object.entries(DOWNLOADS).map(([key, item]) => ({
          id: key,
          filename: item.filename,
        })),
      });
    } catch (err) {
      res.status(500).json({ error: (err as Error).message });
    }
  });

  app.get('/api/matrix', async (_req, res) => {
    try {
      const raw = await fs.readFile(path.join(DOCS_DIR, 'RGMERCS_COMPATIBILITY_MATRIX.json'), 'utf8');
      res.type('application/json').send(raw);
    } catch (err) {
      res.status(404).json({ error: (err as Error).message });
    }
  });

  app.get('/api/doc/:slug', async (req, res) => {
    const spec = DOCS[req.params.slug];
    if (!spec) {
      res.status(404).json({ error: 'Unknown documentation slug' });
      return;
    }
    try {
      const content = await fs.readFile(spec.path, 'utf8');
      res.json({
        slug: req.params.slug,
        title: spec.title,
        filename: spec.filename,
        content,
      });
    } catch (err) {
      res.status(404).json({ error: (err as Error).message });
    }
  });

  app.get('/api/download/:artifact', async (req, res) => {
    const item = DOWNLOADS[req.params.artifact];
    if (!item) {
      res.status(404).json({ error: 'Unknown artifact' });
      return;
    }
    try {
      const body = await fs.readFile(item.path);
      res.setHeader('Content-Type', item.contentType);
      res.setHeader('Content-Disposition', `attachment; filename="${item.filename}"`);
      res.send(body);
    } catch (err) {
      res.status(404).json({ error: (err as Error).message });
    }
  });

  const httpServer = http.createServer(app);

  if (process.env.NODE_ENV === 'production') {
    const distDir = path.resolve(__dirname, 'dist');
    app.use(express.static(distDir));
    app.get('*', (_req, res) => {
      res.sendFile(path.join(distDir, 'index.html'));
    });
  } else {
    const { createServer: createViteServer } = await import('vite');
    const disableHmr = process.env.DISABLE_HMR === 'true';
    const vite = await createViteServer({
      server: {
        middlewareMode: true,
        ws: { server: httpServer },
        hmr: !disableHmr,
        watch: disableHmr ? null : {},
      },
      appType: 'spa',
    });
    app.use(vite.middlewares);
  }

  const port = Number(process.env.PORT ?? 3000);
  httpServer.listen(port, '0.0.0.0', () => {
    console.log(`Hailstone 24 workbench listening on http://0.0.0.0:${port}`);
  });
}

startServer().catch((err) => {
  console.error('Failed to start Hailstone 24 workbench server:', err);
  process.exit(1);
});
