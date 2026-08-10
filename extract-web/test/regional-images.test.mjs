import assert from "node:assert/strict";
import { test } from "node:test";

import {
    BytesSource,
    Vfi,
    decodeTim2,
    locateImageContainers,
    readImageTexture,
    scanImageTextures,
} from "../src/index.ts";
import { buildPck, buildSz, buildVfi } from "./fixtures.mjs";

const enc = new TextEncoder();

/* A single zeroed RGBA32 picture is enough to exercise cataloguing and decode
 * without embedding any game-derived pixels. */
function buildZeroTim2() {
    const pictureSize = 0x30 + 4;
    const out = new Uint8Array(0x10 + pictureSize);
    const view = new DataView(out.buffer);
    out.set(enc.encode("TIM2"));
    out[4] = 4;
    out[6] = 1;
    view.setUint32(0x10, pictureSize, true);
    view.setUint32(0x18, 4, true);
    view.setUint16(0x1c, 0x30, true);
    out[0x21] = 1;
    out[0x23] = 3;
    view.setUint16(0x24, 1, true);
    view.setUint16(0x26, 1, true);
    return out;
}

function buildMalformedTim2() {
    const out = new Uint8Array(0x10);
    out.set(enc.encode("TIM2"));
    out[4] = 4;
    out[6] = 1;
    return out;
}

class ThrowingSource {
    constructor(bytes) {
        this.bytes = bytes;
        this.failOffset = null;
    }

    get size() {
        return this.bytes.length;
    }

    async read(offset, length) {
        if (offset === this.failOffset)
            throw new Error("injected image source read failure");
        return this.bytes.subarray(offset, offset + length);
    }
}

test("images: non-magic tm2 member is skipped while direct and packed images survive", async () => {
    const direct = buildZeroTim2();
    const packed = buildPck([
        { name: "ape_nrm02_b", attrs: "tm2", data: new Uint8Array(16) },
        { name: "regional_sprite", attrs: "tm2", data: buildZeroTim2() },
    ]);
    const vfi = await Vfi.open(new BytesSource(buildVfi([
        { path: "assets/direct.tm2", data: direct },
        { path: "assets/regional.pck", data: packed },
    ])));

    assert.deepEqual(
        locateImageContainers(vfi).map(entry => entry.path).toSorted(),
        ["assets/direct.tm2", "assets/regional.pck"],
    );
    const result = await scanImageTextures(vfi);
    const textures = result.textures;
    assert.deepEqual(result.issues, []);
    assert.equal(textures.length, 2);
    assert.deepEqual(
        textures.map(texture => texture.fileName).toSorted(),
        ["direct.tm2", "regional_sprite.tm2"],
    );

    const directTexture = textures.find(texture => texture.memberIndex === null);
    const packedTexture = textures.find(texture =>
        texture.memberName === "regional_sprite");
    assert.ok(directTexture);
    assert.ok(packedTexture);
    for (const texture of [directTexture, packedTexture]) {
        const image = decodeTim2(await readImageTexture(vfi, texture));
        assert.equal(image.width, 1);
        assert.equal(image.height, 1);
        assert.deepEqual([...image.rgba], [0, 0, 0, 0]);
    }
});

test("images: malformed container is isolated while a valid neighbor and callbacks survive", async () => {
    const broken = buildPck([
        { name: "broken_member", attrs: "tm2", data: buildMalformedTim2() },
    ]);
    const vfi = await Vfi.open(new BytesSource(buildVfi([
        { path: "assets/regional-broken.pck", data: broken },
        { path: "assets/valid-neighbor.tm2", data: buildZeroTim2() },
    ])));
    const seenTextures = [];
    const seenContainers = [];
    const result = await scanImageTextures(vfi, {
        texture: texture => seenTextures.push(texture.fileName),
        container: entry => seenContainers.push(entry.path),
    });

    assert.deepEqual(result.textures.map(texture => texture.fileName),
                     ["valid-neighbor.tm2"]);
    assert.equal(result.issues.length, 1);
    assert.equal(result.issues[0].path, "assets/regional-broken.pck");
    assert.equal(result.issues[0].format, "TIM2");
    assert.match(result.issues[0].reason,
                 /assets\/regional-broken\.pck#0:broken_member:/);
    assert.match(result.issues[0].reason, /picture 0 header exceeds TIM2 data/);
    assert.deepEqual(seenTextures, ["valid-neighbor.tm2"]);
    assert.deepEqual(seenContainers, ["assets/valid-neighbor.tm2"]);
});

test("images: source read failures stay scan-fatal", async () => {
    const source = new ThrowingSource(buildVfi([
        { path: "assets/read-fails.tm2", data: buildZeroTim2() },
    ]));
    const vfi = await Vfi.open(source);
    source.failOffset = vfi.byteOffset(vfi.find("assets/read-fails.tm2"));

    await assert.rejects(
        () => scanImageTextures(vfi),
        /injected image source read failure/,
    );
});

test("images: unexpected inflater failures stay scan-fatal", async () => {
    const packed = buildPck([
        { name: "valid", attrs: "tm2", data: buildZeroTim2() },
    ]);
    const source = new BytesSource(buildVfi([
        { path: "assets/unexpected.pck.sz", data: await buildSz(packed) },
    ]));
    const vfi = await Vfi.open(source);
    const original = globalThis.DecompressionStream;
    globalThis.DecompressionStream = class {
        constructor() {
            throw new Error("injected unexpected inflater failure");
        }
    };
    try {
        await assert.rejects(
            () => scanImageTextures(vfi),
            /injected unexpected inflater failure/,
        );
    } finally {
        globalThis.DecompressionStream = original;
    }
});

test("images: callback failures stay scan-fatal", async () => {
    const vfi = await Vfi.open(new BytesSource(buildVfi([
        { path: "assets/callback-fails.tm2", data: buildZeroTim2() },
    ])));

    await assert.rejects(
        () => scanImageTextures(vfi, {
            container: () => {
                throw new Error("injected image callback failure");
            },
        }),
        /injected image callback failure/,
    );
});
