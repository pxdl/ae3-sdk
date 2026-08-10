import { execFile } from "node:child_process";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { promisify } from "node:util";
import assert from "node:assert/strict";
import { test } from "node:test";

import {
    BytesSource,
    formatDiscSupportMarkdown,
    inspectDiscSupport,
    isDiscSupportReport,
} from "../src/index.ts";
import { buildFmv, buildIso, buildPck, buildVfi } from "./fixtures.mjs";

const encoder = new TextEncoder();
const execFileAsync = promisify(execFile);
const cli = new URL("../bin/ae3-report.mjs", import.meta.url);

function buildTim2() {
    const indices = Uint8Array.of(0);
    const palette = Uint8Array.of(10, 20, 30, 0x80);
    const picture = new Uint8Array(0x30 + indices.length + palette.length);
    const view = new DataView(picture.buffer);
    view.setUint32(0, picture.length, true);
    view.setUint32(4, palette.length, true);
    view.setUint32(8, indices.length, true);
    view.setUint16(12, 0x30, true);
    view.setUint16(14, 1, true);
    picture[17] = 1;
    picture[18] = 0x83;
    picture[19] = 5;
    view.setUint16(20, 1, true);
    view.setUint16(22, 1, true);
    picture.set(indices, 0x30);
    picture.set(palette, 0x31);

    const result = new Uint8Array(0x10 + picture.length);
    result.set(encoder.encode("TIM2"), 0);
    result[4] = 4;
    result[6] = 1;
    result.set(picture, 0x10);
    return result;
}

function supportIso() {
    const effectHeader = new Uint8Array(0x20);
    effectHeader.set(encoder.encode("SShd"), 0x0c);
    const declaredStub = buildPck([{
        name: "declared_stub",
        attrs: "tm2",
        data: Uint8Array.of(1, 2, 3, 4),
    }]);
    const dataBin = buildVfi([
        { path: "debug/us/static/direct.tm2", data: buildTim2() },
        { path: "debug/us/static/stubs.pck", data: declaredStub },
        { path: "debug/us/sound/se/test.hd", data: effectHeader },
        { path: "debug/us/sound/se/test.bd", data: Uint8Array.of(1, 2, 3) },
        { path: "debug/us/movie/new_play01.str", data: buildFmv().bytes },
    ]);
    return buildIso([
        {
            name: "SYSTEM.CNF",
            data: encoder.encode("BOOT2 = cdrom0:\\SCUS_TEST.01;1\r\n"),
        },
        { name: "DATA.BIN", data: dataBin },
    ], "AE3_REPORT_TEST");
}

function failingFamiliesIso() {
    const dataBin = buildVfi([
        { path: "debug/us/static/direct.tm2", data: buildTim2() },
        { path: "debug/us/sound/se/broken.hd", data: Uint8Array.of(1, 2, 3) },
        { path: "debug/us/sound/se/broken.bd", data: Uint8Array.of(4) },
        { path: "debug/us/movie/broken.str", data: Uint8Array.of(5, 6, 7) },
    ]);
    return buildIso([{ name: "DATA.BIN", data: dataBin }], "AE3_REPORT_FAILURE");
}

test("support report: inspects each catalog family and records partial results", async () => {
    const report = await inspectDiscSupport(new BytesSource(supportIso()));

    assert.equal(report.schemaVersion, 1);
    assert.equal(report.disc.serial, "SCUS_TEST.01");
    assert.equal(report.disc.volumeId, "AE3_REPORT_TEST");
    assert.match(report.disc.tableSha256, /^[0-9a-f]{64}$/);
    assert.equal(isDiscSupportReport(report), true);
    const invalid = structuredClone(report);
    invalid.fmv.inspected++;
    assert.equal(isDiscSupportReport(invalid), false);
    assert.deepEqual({
        status: report.images.status,
        containers: report.images.containers,
        textures: report.images.textures,
        pictures: report.images.pictures,
        skipped: report.images.skippedDeclared.length,
    }, {
        status: "partial",
        containers: 2,
        textures: 1,
        pictures: 1,
        skipped: 1,
    });
    assert.deepEqual({
        status: report.effects.status,
        paired: report.effects.pairedBanks,
        inspected: report.effects.inspectedBanks,
    }, { status: "passed", paired: 1, inspected: 1 });
    assert.deepEqual({
        status: report.fmv.status,
        discovered: report.fmv.discovered,
        inspected: report.fmv.inspected,
    }, { status: "passed", discovered: 1, inspected: 1 });
    assert.equal(report.fmv.movies[0]?.name, "new_play01");

    const markdown = formatDiscSupportMarkdown(report, "Synthetic |\nbuild");
    assert.match(markdown, /Synthetic \\| build/);
    assert.match(markdown, /Partial: 1 textures \/ 1 pictures; 1 declared entries/);
    assert.match(markdown, /Passed: 1\/1 paired banks inspected/);
    assert.match(markdown, /Passed: 1\/1 inspected/);
});

test("support report: family failures stay structured and do not abort the report",
     async () => {
    const report = await inspectDiscSupport(
        new BytesSource(failingFamiliesIso()),
    );
    assert.equal(report.disc.serial, null);
    assert.equal(report.images.status, "passed");
    assert.equal(report.effects.status, "failed");
    assert.match(report.effects.issues[0]?.reason ?? "", /SShd/);
    assert.equal(report.fmv.status, "failed");
    assert.equal(report.fmv.discovered, 1);
    assert.equal(report.fmv.inspected, 0);
    assert.equal(report.fmv.issues.length, 1);
    assert.equal(isDiscSupportReport(report), true);
});

test("ae3-report CLI emits its versioned JSON contract", async () => {
    const directory = await mkdtemp(join(tmpdir(), "ae3-report-cli-"));
    try {
        const isoPath = join(directory, "fixture.iso");
        await writeFile(isoPath, supportIso());
        const { stdout, stderr } = await execFileAsync(
            process.execPath,
            [cli.pathname, "--iso", isoPath],
            { maxBuffer: 4 * 1024 * 1024 },
        );
        assert.equal(stderr, "");
        const report = JSON.parse(stdout);
        assert.equal(report.tool, "@ae3/extract/ae3-report");
        assert.equal(report.disc.serial, "SCUS_TEST.01");
        assert.equal(report.images.textures, 1);
        assert.equal(report.effects.inspectedBanks, 1);
        assert.equal(report.fmv.inspected, 1);
        const markdown = await execFileAsync(
            process.execPath,
            [
                cli.pathname,
                "--iso", isoPath,
                "--format", "markdown",
                "--label", "Synthetic build",
            ],
        );
        assert.equal(markdown.stderr, "");
        assert.match(markdown.stdout, /^\| Build \| Images \| Effects \| FMV \|/);
        assert.match(markdown.stdout, /\| Synthetic build \| Partial:/);
    } finally {
        await rm(directory, { recursive: true, force: true });
    }
});
