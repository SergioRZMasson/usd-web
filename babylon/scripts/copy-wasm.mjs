import { cp, mkdir } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const source = resolve(packageRoot, "..", "build", "wasm", "bin");
const destination = resolve(packageRoot, "dist", "wasm");
await mkdir(destination, { recursive: true });
for (const extension of ["js", "wasm", "data"]) {
    await cp(
        resolve(source, `openusd-babylon.${extension}`),
        resolve(destination, `openusd-babylon.${extension}`),
    );
}
