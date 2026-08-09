import { test } from "node:test";
import assert from "node:assert/strict";

import {
    BytesSource, Vfi, parseFmvHeader, inspectFmvPrefix, inspectFmvAsset,
    demuxFmv,
} from "../src/index.ts";
import { buildVfi, buildFmv } from "./fixtures.mjs";

function u32(bytes, offset) {
    return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
        .getUint32(offset, true);
}

function setU32(bytes, offset, value) {
    new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
        .setUint32(offset, value, true);
}

function firstGroupOffset(bytes, lanes) {
    const channels = u32(bytes, 0x24);
    const interleave = u32(bytes, 0x28);
    const preload = u32(bytes, 0x30);
    return lanes === 1
        ? 0x800 + preload
        : 0x800 + lanes * (preload + 2 * channels * interleave);
}

function firstVideoPrefix(bytes, lanes) {
    const videoHeader = firstGroupOffset(bytes, lanes) + 48;
    const videoSize = u32(bytes, videoHeader + 0x14);
    return bytes.subarray(0, videoHeader + 32 + ((videoSize + 15) & ~15));
}

class TrackingSource {
    constructor(bytes) {
        this.bytes = bytes;
        this.size = bytes.length;
        this.reads = [];
    }

    async read(offset, length) {
        this.reads.push({ offset, length });
        return this.bytes.subarray(offset, offset + length);
    }
}

async function openTrackedMovie(movie) {
    const source = new TrackingSource(buildVfi([
        { path: "synthetic/fixture.str", data: movie },
    ]));
    const vfi = await Vfi.open(source);
    source.reads.length = 0;
    return { source, vfi, movie: vfi.entries[0] };
}

const oneLaneInfo = {
    width: 512,
    height: 320,
    frameRate: 30000 / 1001,
    fieldOrder: "progressive",
    sampleAspect: [7, 6],
    displayAspect: [28, 15],
};

const palInfo = {
    width: 512,
    height: 320,
    frameRate: 25,
    fieldOrder: "progressive",
    sampleAspect: [4, 3],
    displayAspect: [32, 15],
};

const expectedHeader = {
    fields: 3,
    fieldRate: 59.94,
    groups: 2,
    sampleRate: 48000,
    channels: 2,
    interleave: 16,
    audioBlock: 32,
    preload: 32,
    audioBytes: 64,
};

function expectedGroups() {
    return [
        { fields: 2, videoChunks: 1, unknown: 7 },
        { fields: 1, videoChunks: 1, unknown: 8 },
    ];
}

test("fmv fixture: existing one-lane behavior remains stable", () => {
    const fixture = buildFmv();
    const fmv = demuxFmv(fixture.bytes, "one-lane fixture");

    assert.deepEqual(fmv.header, expectedHeader);
    assert.deepEqual(fmv.video, fixture.video);
    assert.deepEqual(fmv.groups, expectedGroups());
    assert.deepEqual(fmv.videoInfo, oneLaneInfo);
    assert.equal(fmv.wav.length, 44 + 56 * 2 * 2);
});

test("fmv: PAL five-lane prefix, asset inspection, and demux", async () => {
    const fixture = buildFmv({ lanes: 5, frameRateCode: 3 });
    const prefix = firstVideoPrefix(fixture.bytes, 5);
    const inspected = inspectFmvPrefix(prefix, "PAL five-lane prefix");

    assert.deepEqual(inspected, {
        header: expectedHeader,
        videoInfo: palInfo,
    });

    const vfi = await Vfi.open(new BytesSource(buildVfi([
        { path: "synthetic/fixture.str", data: fixture.bytes },
    ])));
    const asset = await inspectFmvAsset(vfi, vfi.entries[0], "PAL five-lane asset");
    assert.deepEqual(asset, inspected);

    const fmv = demuxFmv(fixture.bytes, "PAL five-lane fixture");
    assert.deepEqual(fmv.header, expectedHeader);
    assert.deepEqual(fmv.video, fixture.video);
    assert.deepEqual(fmv.groups, expectedGroups());
    assert.deepEqual(fmv.videoInfo, palInfo);
    assert.equal(fmv.wav.length, 44 + 56 * 2 * 2);

    /* Lane zero is the delivered stream; the other four lanes are distinct
     * authored ADPCM data and must not change the exact selected audio. */
    const laneZero = demuxFmv(buildFmv({ frameRateCode: 3 }).bytes,
                              "PAL one-lane reference");
    assert.deepEqual(fmv.wav, laneZero.wav);
});

test("fmv: unsupported first-group position is rejected structurally", () => {
    const fixture = buildFmv({ lanes: 5, frameRateCode: 3 });
    const malformed = fixture.bytes.slice();
    const groupOffset = firstGroupOffset(malformed, 5);
    malformed.fill(0, groupOffset, groupOffset + 16);
    const reason = /expected first GroupOfDataInfo at one-track offset 0x820 or five-track offset 0x9e0/;

    assert.throws(() => inspectFmvPrefix(malformed, "bad PAL layout"), reason);
    assert.throws(() => demuxFmv(malformed, "bad PAL layout"), reason);
});

test("fmv: prefix mirrors full first-group field and video-index gates", () => {
    const fixture = buildFmv().bytes;
    const groupOffset = firstGroupOffset(fixture, 1);

    const excessFields = fixture.slice();
    setU32(excessFields, groupOffset + 0x20, 4);
    assert.throws(
        () => inspectFmvPrefix(firstVideoPrefix(excessFields, 1), "excess fields"),
        /group fields end at 4, past header total 3/,
    );

    const nonzeroVideoIndex = fixture.slice();
    setU32(nonzeroVideoIndex, groupOffset + 48 + 0x10, 1);
    assert.throws(
        () => inspectFmvPrefix(
            firstVideoPrefix(nonzeroVideoIndex, 1), "nonzero first video index"),
        /first video index is 1 instead of zero/,
    );
});

test("fmv: asset inspection bounds candidate probes and first video reads", async () => {
    const fixture = buildFmv().bytes;
    const truncatedCandidate = new Uint8Array(0x10000);
    truncatedCandidate.set(fixture.subarray(0, 0x800));
    const preload = truncatedCandidate.length - 0x800 - 32;
    setU32(truncatedCandidate, 0x30, preload);
    setU32(truncatedCandidate, 0x34, preload + 32);
    const candidateAsset = await openTrackedMovie(truncatedCandidate);
    await assert.rejects(
        inspectFmvAsset(
            candidateAsset.vfi, candidateAsset.movie, "truncated group candidate"),
        /range ends .* past movie EOF/,
    );
    assert.ok(candidateAsset.source.reads.every(read => read.length <= 0x800),
              JSON.stringify(candidateAsset.source.reads));

    const oversizedVideo = new Uint8Array(0x20000);
    oversizedVideo.set(fixture);
    const videoHeader = firstGroupOffset(oversizedVideo, 1) + 48;
    setU32(oversizedVideo, videoHeader + 0x14, 0x10010);
    const videoAsset = await openTrackedMovie(oversizedVideo);
    await assert.rejects(
        inspectFmvAsset(videoAsset.vfi, videoAsset.movie, "oversized first video"),
        /first video payload is 65552 bytes, exceeds inspection cap 65536/,
    );
    assert.ok(videoAsset.source.reads.every(read => read.length <= 0x800),
              JSON.stringify(videoAsset.source.reads));
});

test("fmv: proven audio format and WAV allocation bounds fail before writing", () => {
    const fixture = buildFmv().bytes;
    const unsupportedRate = fixture.slice();
    setU32(unsupportedRate, 0x20, 44100);
    assert.throws(
        () => parseFmvHeader(unsupportedRate, "unsupported sample rate"),
        /unsupported sample rate 44100; expected 48000/,
    );

    const unsupportedChannels = fixture.slice();
    setU32(unsupportedChannels, 0x24, 1);
    assert.throws(
        () => parseFmvHeader(unsupportedChannels, "unsupported channels"),
        /unsupported channel count 1; expected stereo/,
    );

    const originalGroup = firstGroupOffset(fixture, 1);
    const oversizedPreload = 0x1280000;
    const oversizedWav = new Uint8Array(
        0x800 + oversizedPreload + fixture.length - originalGroup);
    oversizedWav.set(fixture.subarray(0, 0x800));
    setU32(oversizedWav, 0x30, oversizedPreload);
    setU32(oversizedWav, 0x34, oversizedPreload + 32);
    oversizedWav.set(fixture.subarray(originalGroup), 0x800 + oversizedPreload);
    assert.throws(
        () => demuxFmv(oversizedWav, "oversized WAV"),
        /WAV allocation is \d+ bytes, exceeds cap 67108864/,
    );
});
