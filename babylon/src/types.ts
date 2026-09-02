import type { AssetContainer } from "@babylonjs/core/assetContainer.js";
import type { Scene } from "@babylonjs/core/scene.js";

export type BinaryInput = ArrayBuffer | ArrayBufferView | Uint8Array | Blob;
export type VirtualFiles = Record<string, BinaryInput>;

export interface LoadProgress {
    phase: "initializing" | "staging" | "extracting" | "materializing";
    message: string;
}

export interface LoadOptions {
    fileName?: string;
    files?: VirtualFiles;
    addToScene?: boolean;
    resolveByFileName?: boolean;
    workerUrl?: string | URL;
    wasmUrl?: string | URL;
    onProgress?: (progress: LoadProgress) => void;
    onLog?: (level: "info" | "warning" | "error", message: string) => void;
}

export interface DirectTimings {
    totalMs: number;
    stageOpenMs: number;
    stageReadMs: number;
    preparationMs: number;
    packingMs: number;
    heapCopyMs: number;
    materializeMs: number;
}

export interface SceneStatistics {
    nodes: number;
    meshes: number;
    instances: number;
    materials: number;
    vertices: number;
    triangles: number;
    commandBytes: number;
    dataBytes: number;
}

export interface LoadResult {
    container: AssetContainer;
    timings: DirectTimings;
    statistics: SceneStatistics;
    missingAssets: string[];
}
