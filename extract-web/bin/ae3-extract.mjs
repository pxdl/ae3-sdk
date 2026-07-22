#!/usr/bin/env node

import { createHash } from "node:crypto";
import { dirname, isAbsolute, resolve, sep } from "node:path";
import { mkdir, open, readFile, rename, rm } from "node:fs/promises";
import process from "node:process";

import { ISO_SECTOR, Iso9660, VFI_SECTOR, Vfi } from "../src/index.ts";

const SCHEMA_VERSION = 1;
const READ_CHUNK = 1024 * 1024;
const MAX_RANDOM_READ = 16 * 1024 * 1024;
const MAX_REQUEST_SIZE = 1024 * 1024;
const SCHEMA = Object.freeze({
    requestSchemaVersion: SCHEMA_VERSION,
    manifestSchemaVersion: SCHEMA_VERSION,
    request: {
        required: ["schemaVersion"],
        optionalArrays: ["iso", "vfi"],
        isoEntry: ["path", "output"],
        vfiEntry: ["container", "path", "output"],
    },
});

function usage() {
    return "usage: ae3-extract --iso PATH --request PATH --output DIR --manifest PATH\n"
        + "       ae3-extract --schema\n";
}

function parseArgs(argv) {
    if (argv.length === 1 && argv[0] === "--schema") return { schema: true };
    const values = {};
    for (let index = 0; index < argv.length; index += 2) {
        const flag = argv[index];
        const value = argv[index + 1];
        if (!["--iso", "--request", "--output", "--manifest"].includes(flag) || value === undefined)
            throw new Error(`invalid arguments\n${usage()}`);
        if (values[flag]) throw new Error(`duplicate argument ${flag}`);
        values[flag] = value;
    }
    for (const flag of ["--iso", "--request", "--output", "--manifest"])
        if (!values[flag]) throw new Error(`missing ${flag}\n${usage()}`);
    return {
        schema: false,
        iso: values["--iso"],
        request: values["--request"],
        output: values["--output"],
        manifest: values["--manifest"],
    };
}

function exactKeys(value, required, label) {
    if (!value || typeof value !== "object" || Array.isArray(value))
        throw new Error(`${label} must be an object`);
    const keys = Object.keys(value);
    for (const key of required)
        if (!(key in value)) throw new Error(`${label} missing ${key}`);
    for (const key of keys)
        if (!required.includes(key)) throw new Error(`${label} has unknown key ${key}`);
}

function relativeOutput(value, label) {
    if (typeof value !== "string" || !value || isAbsolute(value))
        throw new Error(`${label} must be a non-empty relative path`);
    const parts = value.split(/[\\/]+/);
    if (parts.some(part => !part || part === "." || part === ".."))
        throw new Error(`${label} must not contain empty, dot, or parent segments`);
    return parts.join("/");
}

function rootName(value, label) {
    if (typeof value !== "string" || !value || value.includes("/") || value.includes("\\") || value === "." || value === "..")
        throw new Error(`${label} must be an ISO root filename`);
    return value;
}

function vfiPath(value, label) {
    if (typeof value !== "string" || !value || isAbsolute(value))
        throw new Error(`${label} must be a non-empty relative VFI path`);
    const parts = value.split(/[\\/]+/);
    if (parts.some(part => !part || part === "." || part === ".."))
        throw new Error(`${label} must not contain empty, dot, or parent segments`);
    return parts.join("/");
}

function validateRequest(value) {
    if (!value || typeof value !== "object" || Array.isArray(value))
        throw new Error("request must be an object");
    const allowed = ["schemaVersion", "iso", "vfi"];
    for (const key of Object.keys(value))
        if (!allowed.includes(key)) throw new Error(`request has unknown key ${key}`);
    if (value.schemaVersion !== SCHEMA_VERSION)
        throw new Error(`request.schemaVersion must be ${SCHEMA_VERSION}`);
    const iso = value.iso ?? [];
    const vfi = value.vfi ?? [];
    if (!Array.isArray(iso) || !Array.isArray(vfi)) throw new Error("request iso and vfi must be arrays");
    if (iso.length + vfi.length === 0) throw new Error("request must contain at least one entry");

    const outputs = new Set();
    const normalizedIso = iso.map((entry, index) => {
        exactKeys(entry, ["path", "output"], `iso[${index}]`);
        const output = relativeOutput(entry.output, `iso[${index}].output`);
        if (outputs.has(output)) throw new Error(`duplicate output ${output}`);
        outputs.add(output);
        return { path: rootName(entry.path, `iso[${index}].path`), output };
    });
    const normalizedVfi = vfi.map((entry, index) => {
        exactKeys(entry, ["container", "path", "output"], `vfi[${index}]`);
        const output = relativeOutput(entry.output, `vfi[${index}].output`);
        if (outputs.has(output)) throw new Error(`duplicate output ${output}`);
        outputs.add(output);
        return {
            container: rootName(entry.container, `vfi[${index}].container`),
            path: vfiPath(entry.path, `vfi[${index}].path`),
            output,
        };
    });
    return { iso: normalizedIso, vfi: normalizedVfi };
}

async function loadRequest(path) {
    const bytes = await readFile(path);
    if (bytes.length > MAX_REQUEST_SIZE) throw new Error(`request exceeds ${MAX_REQUEST_SIZE} bytes`);
    let value;
    try {
        value = JSON.parse(bytes.toString("utf8"));
    } catch (error) {
        throw new Error(`invalid request JSON: ${error.message}`);
    }
    return validateRequest(value);
}

function fileSource(handle, size) {
    return {
        size,
        async read(offset, length) {
            if (!Number.isSafeInteger(offset) || offset < 0 || !Number.isSafeInteger(length) || length < 0)
                throw new Error("read offset and length must be non-negative safe integers");
            if (length > MAX_RANDOM_READ) throw new Error(`random read exceeds ${MAX_RANDOM_READ} bytes`);
            const boundedLength = Math.min(length, Math.max(0, size - Math.min(offset, size)));
            const bytes = new Uint8Array(boundedLength);
            let filled = 0;
            while (filled < boundedLength) {
                const { bytesRead } = await handle.read(bytes, filled, boundedLength - filled, offset + filled);
                if (bytesRead === 0) break;
                filled += bytesRead;
            }
            return filled === bytes.length ? bytes : bytes.subarray(0, filled);
        },
    };
}

async function hashWindow(source, offset, size) {
    const sha1 = createHash("sha1");
    const sha256 = createHash("sha256");
    let position = 0;
    while (position < size) {
        const chunk = await source.read(offset + position, Math.min(READ_CHUNK, size - position));
        if (chunk.length === 0) throw new Error(`short read at ${offset + position}`);
        sha1.update(chunk);
        sha256.update(chunk);
        position += chunk.length;
    }
    return { sha1: sha1.digest("hex"), sha256: sha256.digest("hex") };
}

async function extractWindow(source, offset, size, outputRoot, output) {
    const destination = resolve(outputRoot, output);
    const prefix = outputRoot.endsWith(sep) ? outputRoot : `${outputRoot}${sep}`;
    if (!destination.startsWith(prefix)) throw new Error(`output escapes destination: ${output}`);
    await mkdir(dirname(destination), { recursive: true });
    const partial = `${destination}.partial`;
    const handle = await open(partial, "wx");
    const sha1 = createHash("sha1");
    const sha256 = createHash("sha256");
    let position = 0;
    try {
        while (position < size) {
            const chunk = await source.read(offset + position, Math.min(READ_CHUNK, size - position));
            if (chunk.length === 0) throw new Error(`${output}: short read at ${position} of ${size}`);
            await handle.write(chunk);
            sha1.update(chunk);
            sha256.update(chunk);
            position += chunk.length;
        }
        await handle.sync();
    } catch (error) {
        await handle.close();
        await rm(partial, { force: true });
        throw error;
    }
    await handle.close();
    await rename(partial, destination);
    return { sha1: sha1.digest("hex"), sha256: sha256.digest("hex") };
}

async function writeManifest(path, manifest) {
    await mkdir(dirname(resolve(path)), { recursive: true });
    const partial = `${path}.partial`;
    const handle = await open(partial, "wx");
    try {
        await handle.writeFile(`${JSON.stringify(manifest, null, 2)}\n`, "utf8");
        await handle.sync();
    } catch (error) {
        await handle.close();
        await rm(partial, { force: true });
        throw error;
    }
    await handle.close();
    await rename(partial, path);
}

async function run(options) {
    const request = await loadRequest(options.request);
    const input = await open(options.iso, "r");
    try {
        const stat = await input.stat();
        if (!stat.isFile()) throw new Error("--iso must name a regular file");
        const source = fileSource(input, stat.size);
        const iso = await Iso9660.open(source);
        const outputRoot = resolve(options.output);
        await mkdir(outputRoot, { recursive: true });

        const entries = [];
        for (const requested of request.iso) {
            const entry = await iso.findRoot(requested.path);
            if (!entry || entry.isDir) throw new Error(`ISO entry not found: ${requested.path}`);
            const absoluteByteOffset = entry.lba * ISO_SECTOR;
            const hashes = await extractWindow(source, absoluteByteOffset, entry.size, outputRoot, requested.output);
            entries.push({
                kind: "iso",
                path: entry.name,
                output: requested.output,
                lba: entry.lba,
                absoluteByteOffset,
                size: entry.size,
                ...hashes,
            });
        }

        const vfiCache = new Map();
        for (const requested of request.vfi) {
            let located = vfiCache.get(requested.container.toUpperCase());
            if (!located) {
                const container = await iso.findRoot(requested.container);
                if (!container || container.isDir) throw new Error(`VFI container not found: ${requested.container}`);
                const containerOffset = container.lba * ISO_SECTOR;
                const window = {
                    size: container.size,
                    read(offset, length) {
                        const boundedOffset = Math.min(offset, container.size);
                        return source.read(containerOffset + boundedOffset, Math.min(length, container.size - boundedOffset));
                    },
                };
                located = { container, containerOffset, window, vfi: await Vfi.open(window) };
                vfiCache.set(requested.container.toUpperCase(), located);
            }
            const entry = located.vfi.find(requested.path);
            if (!entry) throw new Error(`VFI entry not found: ${requested.path}`);
            const vfiByteOffset = entry.sector * VFI_SECTOR;
            const absoluteByteOffset = located.containerOffset + vfiByteOffset;
            const hashes = await extractWindow(located.window, vfiByteOffset, entry.size, outputRoot, requested.output);
            entries.push({
                kind: "vfi",
                container: located.container.name,
                containerLba: located.container.lba,
                containerAbsoluteByteOffset: located.containerOffset,
                path: entry.path,
                output: requested.output,
                sector: entry.sector,
                vfiByteOffset,
                absoluteByteOffset,
                size: entry.size,
                ...hashes,
            });
        }

        const sourceHashes = await hashWindow(source, 0, stat.size);
        await writeManifest(options.manifest, {
            schemaVersion: SCHEMA_VERSION,
            tool: "@ae3/extract/ae3-extract",
            source: { size: stat.size, ...sourceHashes },
            iso: { volumeId: iso.volumeId, sectorSize: ISO_SECTOR },
            entries,
        });
    } finally {
        await input.close();
    }
}

try {
    const options = parseArgs(process.argv.slice(2));
    if (options.schema) process.stdout.write(`${JSON.stringify(SCHEMA)}\n`);
    else await run(options);
} catch (error) {
    process.stderr.write(`ae3-extract: ${error.message}\n`);
    process.exitCode = 1;
}
