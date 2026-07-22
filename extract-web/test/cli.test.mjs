import { createHash } from "node:crypto";
import { execFile } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { promisify } from "node:util";
import assert from "node:assert/strict";
import { test } from "node:test";

import { buildIso, buildVfi } from "./fixtures.mjs";

const execFileAsync = promisify(execFile);
const cli = new URL("../bin/ae3-extract.mjs", import.meta.url);
const encoder = new TextEncoder();

function digest(algorithm, bytes) {
    return createHash(algorithm).update(bytes).digest("hex");
}

async function fixture() {
    const directory = await mkdtemp(join(tmpdir(), "ae3-extract-cli-"));
    const systemCnf = encoder.encode("BOOT2 = cdrom0:\\SCUS_TEST.01;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n");
    const boot = Uint8Array.from([0x7f, 0x45, 0x4c, 0x46, 1, 2, 3, 4]);
    const module = encoder.encode("synthetic sg2 module");
    const dataBin = buildVfi([
        { path: "irx/3.0/sg2iopm1.irx", data: module },
        { path: "debug/us/unused.bin", data: Uint8Array.from([9, 8, 7]) },
    ]);
    const iso = buildIso([
        { name: "SYSTEM.CNF", data: systemCnf },
        { name: "SCUS_TEST.01", data: boot },
        { name: "DATA.BIN", data: dataBin },
    ], "AE3_CLI_TEST");
    const isoPath = join(directory, "fixture.iso");
    await writeFile(isoPath, iso);
    return { directory, isoPath, iso, systemCnf, boot, module };
}

async function invoke(paths, request) {
    const requestPath = join(paths.directory, "request.json");
    const outputPath = join(paths.directory, "output");
    const manifestPath = join(paths.directory, "locator.json");
    await writeFile(requestPath, `${JSON.stringify(request)}\n`);
    await execFileAsync(process.execPath, [
        cli.pathname,
        "--iso", paths.isoPath,
        "--request", requestPath,
        "--output", outputPath,
        "--manifest", manifestPath,
    ]);
    return { outputPath, manifestPath };
}

test("cli: schema contract is versioned", async () => {
    const { stdout, stderr } = await execFileAsync(process.execPath, [cli.pathname, "--schema"]);
    assert.equal(stderr, "");
    const schema = JSON.parse(stdout);
    assert.equal(schema.requestSchemaVersion, 1);
    assert.equal(schema.manifestSchemaVersion, 1);
    assert.deepEqual(schema.request.isoEntry, ["path", "output"]);
    assert.deepEqual(schema.request.vfiEntry, ["container", "path", "output"]);
});

test("cli: extracts ISO and nested VFI entries with stable locators and hashes", async () => {
    const paths = await fixture();
    try {
        const { outputPath, manifestPath } = await invoke(paths, {
            schemaVersion: 1,
            iso: [
                { path: "system.cnf", output: "input/SYSTEM.CNF" },
                { path: "SCUS_TEST.01", output: "input/SCUS_TEST.01" },
            ],
            vfi: [
                { container: "data.bin", path: "irx/3.0/sg2iopm1.irx", output: "input/sg2iopm1.irx" },
            ],
        });
        assert.deepEqual(new Uint8Array(await readFile(join(outputPath, "input/SYSTEM.CNF"))), paths.systemCnf);
        assert.deepEqual(new Uint8Array(await readFile(join(outputPath, "input/SCUS_TEST.01"))), paths.boot);
        assert.deepEqual(new Uint8Array(await readFile(join(outputPath, "input/sg2iopm1.irx"))), paths.module);

        const manifest = JSON.parse(await readFile(manifestPath, "utf8"));
        assert.equal(manifest.schemaVersion, 1);
        assert.equal(manifest.tool, "@ae3/extract/ae3-extract");
        assert.deepEqual(manifest.source, {
            size: paths.iso.length,
            sha1: digest("sha1", paths.iso),
            sha256: digest("sha256", paths.iso),
        });
        assert.deepEqual(manifest.iso, { volumeId: "AE3_CLI_TEST", sectorSize: 2048 });
        assert.equal(manifest.entries.length, 3);
        assert.deepEqual(manifest.entries.map(entry => entry.kind), ["iso", "iso", "vfi"]);
        for (const entry of manifest.entries) {
            const bytes = await readFile(join(outputPath, entry.output));
            assert.equal(entry.size, bytes.length);
            assert.equal(entry.sha1, digest("sha1", bytes));
            assert.equal(entry.sha256, digest("sha256", bytes));
        }
        const nested = manifest.entries[2];
        assert.equal(nested.vfiByteOffset, nested.sector * 2048);
        assert.equal(nested.absoluteByteOffset, nested.containerAbsoluteByteOffset + nested.vfiByteOffset);
    } finally {
        await rm(paths.directory, { recursive: true, force: true });
    }
});

test("cli: rejects output traversal before extracting", async () => {
    const paths = await fixture();
    try {
        await assert.rejects(
            invoke(paths, {
                schemaVersion: 1,
                iso: [{ path: "SYSTEM.CNF", output: "../escaped.cnf" }],
            }),
            error => {
                assert.match(error.stderr, /must not contain .* parent segments/);
                return true;
            },
        );
    } finally {
        await rm(paths.directory, { recursive: true, force: true });
    }
});

test("cli: rejects non-object requests", async () => {
    const paths = await fixture();
    try {
        await assert.rejects(
            invoke(paths, null),
            error => {
                assert.match(error.stderr, /request must be an object/);
                return true;
            },
        );
    } finally {
        await rm(paths.directory, { recursive: true, force: true });
    }
});
