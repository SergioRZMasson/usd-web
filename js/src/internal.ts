/**
 * Internal helpers. Not exported from the package entry point.
 */

import { InvalidInputError } from './errors.js';
import type { BinaryInput } from './types.js';

/** Extensions the native importer understands. */
export const SUPPORTED_EXTENSIONS = ['usd', 'usda', 'usdc', 'usdz'] as const;

/**
 * Normalises any accepted binary input into a `Uint8Array` without copying
 * when avoidable.
 */
export function toUint8Array(input: BinaryInput, label = 'input'): Uint8Array {
    if (input instanceof Uint8Array) {
        return input;
    }
    if (input instanceof ArrayBuffer) {
        return new Uint8Array(input);
    }
    if (ArrayBuffer.isView(input)) {
        return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    }
    throw new InvalidInputError(
        `Expected ${label} to be an ArrayBuffer, a typed array or a DataView, but received ${describe(input)}.`,
    );
}

/** Produces a human-readable description of an arbitrary value for errors. */
export function describe(value: unknown): string {
    if (value === null) return 'null';
    if (value === undefined) return 'undefined';
    const type = typeof value;
    if (type === 'object') {
        const name = (value as object).constructor?.name;
        return name ? `an instance of ${name}` : 'a plain object';
    }
    return `a ${type}`;
}

/** Extracts a lowercase extension without the leading dot. */
export function extensionOf(fileName: string): string {
    const dot = fileName.lastIndexOf('.');
    return dot === -1 ? '' : fileName.slice(dot + 1).toLowerCase();
}

/** Replaces the extension of `fileName` with `extension`. */
export function withExtension(fileName: string, extension: string): string {
    const dot = fileName.lastIndexOf('.');
    const stem = dot === -1 ? fileName : fileName.slice(0, dot);
    return `${stem}.${extension}`;
}

/**
 * Validates that `fileName` carries an extension the importer supports, and
 * fails loudly otherwise — a mismatched extension otherwise surfaces as an
 * opaque native parse error.
 */
export function assertSupportedExtension(fileName: string): void {
    const extension = extensionOf(fileName);
    if (!extension) {
        throw new InvalidInputError(
            `The file name "${fileName}" has no extension. The extension selects the importer, ` +
                `so it must be one of: ${SUPPORTED_EXTENSIONS.join(', ')}.`,
        );
    }
    if (!(SUPPORTED_EXTENSIONS as readonly string[]).includes(extension)) {
        throw new InvalidInputError(
            `Unsupported input format ".${extension}". Supported formats are: ` +
                `${SUPPORTED_EXTENSIONS.map((e) => `.${e}`).join(', ')}.`,
        );
    }
}

/**
 * Sanitises a caller-supplied virtual path so it cannot escape the scratch
 * directory used inside the Emscripten filesystem.
 */
export function sanitizeVirtualPath(path: string): string {
    const normalized = path.replace(/\\/g, '/');
    const segments: string[] = [];
    for (const segment of normalized.split('/')) {
        if (segment === '' || segment === '.') continue;
        if (segment === '..') {
            segments.pop();
            continue;
        }
        segments.push(segment);
    }
    if (segments.length === 0) {
        throw new InvalidInputError(`"${path}" is not a usable virtual file path.`);
    }
    return segments.join('/');
}
