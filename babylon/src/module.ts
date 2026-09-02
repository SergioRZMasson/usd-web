export interface NativeDirectResult {
    ok(): boolean;
    error(): string;
    commandPtr(): number;
    commandSize(): number;
    dataPtr(): number;
    dataSize(): number;
    totalMs(): number;
    stageOpenMs(): number;
    stageReadMs(): number;
    preparationMs(): number;
    packingMs(): number;
    nodeCount(): number;
    meshCount(): number;
    instanceCount(): number;
    materialCount(): number;
    vertexCount(): number;
    triangleCount(): number;
    delete(): void;
}

export interface DirectModule {
    HEAPU8: Uint8Array;
    FS: {
        writeFile(path: string, data: Uint8Array): void;
        unlink(path: string): void;
        mkdir(path: string): void;
        rmdir(path: string): void;
        analyzePath(path: string): { exists: boolean };
    };
    extract(path: string): NativeDirectResult;
    setLogCallback(callback: (level: number, message: string) => void): void;
    registerAssetDirectory(path: string): void;
    clearAssetIndex(): void;
    setAssetFallbackEnabled(enabled: boolean): void;
    getUnresolvedAssets(): string;
}

export type DirectModuleFactory = (
    options?: {
        locateFile?: (path: string, scriptDirectory: string) => string;
        noExitRuntime?: boolean;
    },
) => Promise<DirectModule>;
