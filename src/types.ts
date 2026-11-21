export type ExecutionState =
  | 'FOREGROUND_FULL'
  | 'BACKGROUND_HEADLESS'
  | 'TRANSITIONING'
  | 'DISCONNECTED';

export type GpuRetentionMode = 'cached' | 'deferred' | 'shared-broker';

export interface OverviewResponse {
  project: string;
  targetServer: string;
  workstation: {
    os: string;
    gpu: string;
    vramGb: number;
    ramGb: number;
  };
  patch: {
    exists: boolean;
    bytes: number;
    filesChanged: number;
    insertions: number;
    deletions: number;
    files: string[];
  };
  compatibilityCounts: {
    total: number;
    supported: number;
    partial: number;
    unsupported: number;
  };
  artifacts: Array<{
    id: string;
    filename: string;
  }>;
}

export interface MatrixEntry {
  id: string;
  category: string;
  symbol: string;
  requirement: string;
  status: 'Supported' | 'Partial' | 'Unsupported';
  implementation: string;
  notes: string;
}

export interface MatrixResponse {
  project: string;
  target: string;
  generated_at: string;
  entries: MatrixEntry[];
}

export interface DocResponse {
  slug: string;
  title: string;
  filename: string;
  content: string;
}

export interface SimSessionSlot {
  slotIndex: number;
  sessionId: string;
  character: string;
  account: string;
  classShort: string;
  role: 'Tank' | 'Healer' | 'Support' | 'DPS';
  groupNumber: 1 | 2 | 3 | 4;
  zone: string;
  state: ExecutionState;
  rgMercsState: 'Running' | 'Paused' | 'Idle';
}
