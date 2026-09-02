import type { LoadProgress } from "./types.js";

export interface WorkerAsset {
    bytes: Uint8Array;
    fileName: string;
    files?: Record<string, Uint8Array>;
    resolveByFileName: boolean;
    wasmUrl?: string;
}

export interface ExtractRequest {
    type: "extract";
    requestId: number;
    asset: WorkerAsset;
}

export interface WorkerTimings {
    totalMs: number;
    stageOpenMs: number;
    stageReadMs: number;
    preparationMs: number;
    packingMs: number;
    heapCopyMs: number;
}

export interface WorkerStatistics {
    nodes: number;
    meshes: number;
    instances: number;
    materials: number;
    vertices: number;
    triangles: number;
    commandBytes: number;
    dataBytes: number;
}

export type WorkerResponse =
    | { type: "progress"; requestId: number; progress: LoadProgress }
    | { type: "log"; requestId: number; level: number; message: string }
    | {
          type: "result";
          requestId: number;
          commands: ArrayBuffer;
          data: ArrayBuffer;
          timings: WorkerTimings;
          statistics: WorkerStatistics;
          missingAssets: string[];
      }
    | { type: "error"; requestId: number; message: string; stack?: string };
