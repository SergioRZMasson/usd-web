import type { Scene } from "@babylonjs/core/scene.js";

import { materializeCommandBuffers } from "./materialize.js";
import type {
    BinaryInput,
    LoadOptions,
    LoadProgress,
    LoadResult,
    VirtualFiles,
} from "./types.js";
import type { ExtractRequest, WorkerResponse } from "./workerMessages.js";

export type {
    BinaryInput,
    DirectTimings,
    LoadOptions,
    LoadProgress,
    LoadResult,
    SceneStatistics,
    VirtualFiles,
} from "./types.js";
export { materializeCommandBuffers } from "./materialize.js";

interface PendingRequest {
    resolve: (response: Extract<WorkerResponse, { type: "result" }>) => void;
    reject: (error: Error) => void;
    onProgress?: (progress: LoadProgress) => void;
    onLog?: LoadOptions["onLog"];
}

async function toBytes(input: BinaryInput): Promise<Uint8Array> {
    if (input instanceof Blob) {
        return new Uint8Array(await input.arrayBuffer());
    }
    if (input instanceof Uint8Array) {
        return input.slice();
    }
    if (input instanceof ArrayBuffer) {
        return new Uint8Array(input.slice(0));
    }
    return new Uint8Array(input.buffer.slice(input.byteOffset, input.byteOffset + input.byteLength));
}

async function normalizeFiles(files?: VirtualFiles): Promise<Record<string, Uint8Array> | undefined> {
    if (!files) return undefined;
    const normalized: Record<string, Uint8Array> = {};
    for (const [path, input] of Object.entries(files)) {
        normalized[path] = await toBytes(input);
    }
    return normalized;
}

export class OpenUsdBabylonLoader {
    #worker: Worker | undefined;
    #nextRequestId = 1;
    #pending = new Map<number, PendingRequest>();

    async loadAsync(
        scene: Scene,
        input: BinaryInput,
        options: LoadOptions = {},
    ): Promise<LoadResult> {
        const bytes = await toBytes(input);
        const files = await normalizeFiles(options.files);
        const requestId = this.#nextRequestId++;
        const request: ExtractRequest = {
            type: "extract",
            requestId,
            asset: {
                bytes,
                files,
                fileName: options.fileName ?? "scene.usdz",
                resolveByFileName: options.resolveByFileName ?? true,
                wasmUrl: options.wasmUrl ? String(options.wasmUrl) : undefined,
            },
        };
        const transfer = [
            ...new Set([
                bytes.buffer,
                ...Object.values(files ?? {}).map((file) => file.buffer),
            ]),
        ];
        const response = await new Promise<Extract<WorkerResponse, { type: "result" }>>(
            (resolve, reject) => {
                this.#pending.set(requestId, {
                    resolve,
                    reject,
                    onProgress: options.onProgress,
                    onLog: options.onLog,
                });
                try {
                    this.#getWorker(options.workerUrl).postMessage(request, transfer);
                } catch (error) {
                    this.#pending.delete(requestId);
                    reject(error instanceof Error ? error : new Error(String(error)));
                }
            },
        );

        options.onProgress?.({
            phase: "materializing",
            message: "Creating Babylon.js objects...",
        });
        const materialized = await materializeCommandBuffers(
            scene,
            response.commands,
            response.data,
            options.addToScene ?? true,
        );
        return {
            container: materialized.container,
            timings: {
                ...response.timings,
                materializeMs: materialized.materializeMs,
            },
            statistics: response.statistics,
            missingAssets: response.missingAssets,
        };
    }

    dispose(): void {
        this.#worker?.terminate();
        this.#worker = undefined;
        const error = new Error("OpenUsdBabylonLoader was disposed.");
        for (const request of this.#pending.values()) {
            request.reject(error);
        }
        this.#pending.clear();
    }

    #getWorker(workerUrl?: string | URL): Worker {
        if (this.#worker) return this.#worker;
        this.#worker = new Worker(
            workerUrl ? String(workerUrl) : new URL("./worker.js", import.meta.url),
            { type: "module" },
        );
        this.#worker.addEventListener("message", (event: MessageEvent<WorkerResponse>) => {
            const response = event.data;
            const pending = this.#pending.get(response.requestId);
            if (!pending) return;
            if (response.type === "progress") {
                pending.onProgress?.(response.progress);
                return;
            }
            if (response.type === "log") {
                pending.onLog?.(
                    response.level >= 2 ? "error" : response.level === 1 ? "warning" : "info",
                    response.message,
                );
                return;
            }
            this.#pending.delete(response.requestId);
            if (response.type === "error") {
                const error = new Error(response.message);
                error.stack = response.stack;
                pending.reject(error);
            } else {
                pending.resolve(response);
            }
        });
        const fail = (event: ErrorEvent | MessageEvent) => {
            const error =
                event instanceof ErrorEvent
                    ? event.error ?? new Error(event.message)
                    : new Error("The OpenUSD worker could not deserialize a message.");
            this.#worker?.terminate();
            this.#worker = undefined;
            for (const request of this.#pending.values()) {
                request.reject(error);
            }
            this.#pending.clear();
        };
        this.#worker.addEventListener("error", fail);
        this.#worker.addEventListener("messageerror", fail);
        return this.#worker;
    }
}

let defaultLoader: OpenUsdBabylonLoader | undefined;

export async function loadUsdIntoSceneAsync(
    scene: Scene,
    input: BinaryInput,
    options?: LoadOptions,
): Promise<LoadResult> {
    defaultLoader ??= new OpenUsdBabylonLoader();
    return await defaultLoader.loadAsync(scene, input, options);
}
