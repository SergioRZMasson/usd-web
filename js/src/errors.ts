/**
 * Error types raised by babylon-polymorph.
 *
 * Every failure surfaces as a {@link PolymorphError} subclass so callers can
 * discriminate without string matching.
 */

import type { LogMessage } from './types.js';

/** Base class for all errors thrown by this package. */
export class PolymorphError extends Error {
    constructor(message: string, options?: { cause?: unknown }) {
        super(message, options);
        this.name = new.target.name;
        // Restores the prototype chain when compiled to ES5 targets.
        Object.setPrototypeOf(this, new.target.prototype);
    }
}

/** The WebAssembly module could not be fetched or instantiated. */
export class ModuleLoadError extends PolymorphError {
    constructor(message: string, options?: { cause?: unknown }) {
        super(`Failed to load the WebAssembly module: ${message}`, options);
    }
}

/** The supplied input was rejected before reaching the native importer. */
export class InvalidInputError extends PolymorphError {}

/** The native importer failed to parse or convert the asset. */
export class ConversionError extends PolymorphError {
    /** Diagnostics captured while the conversion was running. */
    readonly log: readonly LogMessage[];

    constructor(message: string, log: readonly LogMessage[] = [], options?: { cause?: unknown }) {
        super(message, options);
        this.log = log;
    }
}

/** The conversion was aborted through an {@link AbortSignal}. */
export class ConversionAbortedError extends PolymorphError {
    constructor() {
        super('The conversion was aborted.');
    }
}

/**
 * The WebAssembly module called `abort()` and can no longer be used.
 *
 * OpenUSD treats some malformed input as a fatal error, which in a wasm build terminates
 * the runtime rather than unwinding. The instance is unrecoverable; create a new
 * converter to continue.
 */
export class ModuleAbortedError extends PolymorphError {
    constructor(reason?: string) {
        super(
            'The WebAssembly module aborted' +
                (reason ? `: ${reason}` : '') +
                '. This instance can no longer be used — create a new converter.',
        );
    }
}

/** A method was called on a converter that has already been disposed. */
export class DisposedError extends PolymorphError {
    constructor() {
        super('This UsdConverter has been disposed and can no longer be used.');
    }
}
