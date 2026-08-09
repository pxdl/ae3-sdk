/* Public gate for @ae3/extract: every parser against synthetic fixtures,
 * plus the end-to-end openDisc path over a miniature disc image. The
 * real-corpus byte-equality gate is private (checks/extract_ab.mjs in the
 * research repo -- it needs a disc). */

import { test } from "node:test";
import assert from "node:assert/strict";

import {
    BytesSource, Iso9660, systemCnfSerial, Vfi, inflateSz,
    unpackPck, memberBytes, typeOf, attrsOf, safeMember, pckFileNames,
    inspectTim2, decodeTim2, locateImageContainers, scanImageTextures,
    readImageTexture, locateModelContainers, scanModelAssets,
    decodeI3dModel, decodeI3dAnimation, decodeI3dCollision,
    parseExdb, bgmDescRecords, bgmSongTable, natcmp, sniff, openDisc,
    locateFmvAssets, parseFmvHeader, inspectFmvPrefix, inspectFmvAsset,
    demuxFmv, indexMpeg2SeekPoints, parseFmvSubtitles,
    subtitlesToSrt, subtitlesToVtt,
} from "../src/index.ts";
import { FmvFormatError } from "../src/fmv.ts";
import {
    buildSz, buildPck, buildExdb, buildVfi, buildIso, buildFmv,
    buildFmvSubtitles,
} from "./fixtures.mjs";
const enc = new TextEncoder();
const bytes = (...v) => Uint8Array.from(v);

function buildTim2(pictures) {
    const pictureBytes = pictures.map(({ width, height, indices, palette,
                                         clutType = 0x83 }) => {
        const imageSize = indices.length;
        const clutSize = palette.length;
        const out = new Uint8Array(0x30 + imageSize + clutSize);
        const view = new DataView(out.buffer);
        view.setUint32(0, out.length, true);
        view.setUint32(4, clutSize, true);
        view.setUint32(8, imageSize, true);
        view.setUint16(12, 0x30, true);
        view.setUint16(14, clutSize / 4, true);
        out[17] = 1;
        out[18] = clutType;
        out[19] = 5;                    // IDTEX8
        view.setUint16(20, width, true);
        view.setUint16(22, height, true);
        out.set(indices, 0x30);
        out.set(palette, 0x30 + imageSize);
        return out;
    });
    const total = 0x10 + pictureBytes.reduce((sum, picture) => sum + picture.length, 0);
    const out = new Uint8Array(total);
    out.set(enc.encode("TIM2"), 0);
    out[4] = 4;
    out[6] = pictures.length & 0xff;
    out[7] = pictures.length >> 8;
    let offset = 0x10;
    for (const picture of pictureBytes) {
        out.set(picture, offset);
        offset += picture.length;
    }
    return out;
}

function buildEmptyI3d() {
    const out = new Uint8Array(0x40);
    const view = new DataView(out.buffer);
    out.set(enc.encode("I3D_BIN\0"), 0);
    view.setUint32(8, 0x00100001, true);
    view.setUint32(0x10, 0x10, true);
    view.setUint32(0x14, 0x52000000, true);
    return out;
}

function buildLitI3d() {
    const out = new Uint8Array(0x500);
    const view = new DataView(out.buffer);
    out.set(enc.encode("I3D_BIN\0"), 0);
    view.setUint32(8, 0x00100001, true);

    const node = (offset, type, data = 0, children = 0, count = 0) => {
        view.setUint32(offset, data === 0 ? 0 : data - 0x10, true);
        view.setUint32(offset + 4, type * 0x01000000 + count, true);
        view.setUint32(offset + 8, children === 0 ? 0 : children - 0x10, true);
    };
    node(0x10, 0x52, 0x210, 0x30, 2);
    node(0x30, 0x2a, 0x2a0, 0x50, 1);
    node(0x40, 0x2d, 0, 0x60, 2);
    node(0x50, 0x59, 0x2b0);
    node(0x60, 0x46);
    node(0x70, 0x4b, 0, 0x80, 1);
    node(0x80, 0x4d, 0x2d0, 0x90, 1);
    node(0x90, 0x56, 0, 0xa0, 6);
    node(0xa0, 0x02, 0, 0x120, 1);
    node(0xb0, 0x03);
    node(0xc0, 0x03, 0, 0x130, 1);
    node(0xd0, 0x37);
    node(0xe0, 0x03, 0, 0x140, 1);
    node(0xf0, 0x02);
    node(0x120, 0x30, 0, 0x150, 1);
    node(0x130, 0x33, 0x300);
    node(0x140, 0x33, 0x350);
    node(0x150, 0x02, 0, 0x160, 1);
    node(0x160, 0x47, 0x3a0, 0x170, 2);
    node(0x170, 0x03);
    node(0x180, 0x03, 0, 0x190, 1);
    node(0x190, 0x33, 0x3d0);

    view.setUint32(0x210 + 0x14, 0x40, true);
    view.setUint32(0x210 + 0x1c, 0x80, true);
    for (let index = 0; index < 16; index++)
        view.setFloat32(0x250 + index * 4, index % 5 === 0 ? 1 : 0, true);
    view.setUint16(0x290, 0, true);
    out.set(enc.encode("root\0"), 0x292);
    view.setUint16(0x2a0, 0xffff, true);
    view.setUint32(0x2b0, 0x10, true);
    view.setUint16(0x2b4, 0, true);
    view.setUint16(0x2b6, 1, true);
    view.setUint16(0x2c0, 0, true);

    const vectors = (offset, rows) => {
        out[offset + 6] = rows.length;
        rows.forEach((row, rowIndex) => row.forEach((value, component) =>
            view.setFloat32(offset + 0x10 + rowIndex * 0x10 + component * 4,
                            value, true)));
    };
    vectors(0x300, [[0, 1, 0, 0], [1, 0, 0, 0], [0, 0, 1, 0]]);
    vectors(0x350, [[0, 0, 0, 1], [1, 0, 0, 1], [0, 1, 0, 1]]);
    out[0x3a5] = 3;
    out.set(bytes(0, 0x80, 0, 1, 1, 0x80, 0, 1, 2, 0, 0, 1), 0x3b0);
    vectors(0x3d0, [[0, 0, 0, 0], [1, 0, 0, 0], [0, 1, 0, 0]]);
    return out;
}

/* ---- vfi ---------------------------------------------------------------- */

test("vfi: paths, offsets, payloads", async () => {
    const files = [
        { path: "root.bin", data: bytes(1, 2, 3) },
        { path: "irx/3.0/libsd.irx", data: enc.encode("\x7fELF-ish") },
        { path: "debug/us/sound/bgm/s_9.mid", data: enc.encode("MThd fake") },
        { path: "debug/us/sound/bgm/s_9.hd", data: enc.encode("SShd fake") },
        { path: "empty.bin", data: bytes() },
    ];
    const vfi = await Vfi.open(new BytesSource(buildVfi(files)));
    assert.equal(vfi.entries.length, files.length);
    assert.deepEqual(vfi.entries.map(e => e.path), files.map(f => f.path));
    for (let i = 0; i < files.length; i++) {
        assert.equal(vfi.entries[i].size, files[i].data.length);
        assert.deepEqual(await vfi.read(vfi.entries[i]), files[i].data);
    }
    /* deep chains share folder records: both bgm files, one parent chain */
    assert.equal(vfi.entries[2].parentOff, vfi.entries[3].parentOff);
    assert.equal(vfi.find("irx/3.0/libsd.irx"), vfi.entries[1]);
    assert.equal(vfi.find("nope"), null);
});

test("vfi: bad magic rejected", async () => {
    await assert.rejects(Vfi.open(new BytesSource(new Uint8Array(64))),
                         /bad VFI magic/);
});

/* ---- fmv ---------------------------------------------------------------- */

test("fmv: region-tolerant discovery, subtitle pairing, blank sentinel", async () => {
    const movie = buildFmv().bytes;
    const { bin, sbt } = buildFmvSubtitles();
    const vfi = await Vfi.open(new BytesSource(buildVfi([
        { path: "debug/jp/movie/new_scene01.str", data: movie },
        { path: "debug/us/movie/new_scene01.str", data: movie },
        { path: "debug/us/movie/scene01.bin", data: bin },
        { path: "debug/us/movie/scene01.sbt", data: sbt },
        { path: "debug/us/movie/new_play01.str", data: movie },
        { path: "debug/us/movie/blank.bin", data: bin },
        { path: "debug/us/movie/blank.sbt", data: sbt },
    ])));
    const assets = locateFmvAssets(vfi);
    assert.deepEqual(assets.map(asset => asset.name), ["new_play01", "new_scene01"]);
    assert.equal(assets[0].subtitleBin, null);
    assert.equal(assets[1].subtitleBin?.name, "scene01.bin");
    assert.equal(assets[1].subtitleSbt?.name, "scene01.sbt");
    assert.ok(!assets.some(asset => asset.name.includes("blank")));
    assert.deepEqual((await inspectFmvAsset(vfi, assets[0].movie)).videoInfo, {
        width: 512,
        height: 320,
        frameRate: 30000 / 1001,
        fieldOrder: "progressive",
        sampleAspect: [7, 6],
        displayAspect: [28, 15],
    });
});

test("fmv: incomplete subtitle pairs are scoped discovery issues", async () => {
    const movie = buildFmv().bytes;
    const { bin } = buildFmvSubtitles();
    const vfi = await Vfi.open(new BytesSource(buildVfi([
        { path: "debug/us/movie/new_play01.str", data: movie },
        { path: "debug/us/movie/new_scene01.str", data: movie },
        { path: "debug/us/movie/scene01.bin", data: bin },
    ])));

    const discoveries = locateFmvAssets(vfi);
    const playable = discoveries.find(asset => asset.name === "new_play01");
    const incomplete = discoveries.find(asset => asset.name === "new_scene01");
    assert.equal(playable?.subtitleBin, null);
    assert.ok(incomplete?.formatError instanceof FmvFormatError);
    assert.match(incomplete.formatError.message, /incomplete subtitle pair/);
});

test("fmv: progressive demux, odd fields, audio alignment and predictor history", () => {
    const fixture = buildFmv();
    const header = parseFmvHeader(fixture.bytes.subarray(0, 0x800), "fixture");
    assert.equal(header.fields, 3);
    assert.equal(header.fieldRate, 59.94);
    assert.equal(header.audioBytes, 64);
    assert.deepEqual(inspectFmvPrefix(fixture.bytes, "fixture"), {
        header,
        videoInfo: {
            width: 512,
            height: 320,
            frameRate: 30000 / 1001,
            fieldOrder: "progressive",
            sampleAspect: [7, 6],
            displayAspect: [28, 15],
        },
    });

    const fmv = demuxFmv(fixture.bytes, "fixture");
    assert.deepEqual(fmv.video, fixture.video);
    assert.deepEqual(fmv.groups.map(group => group.fields), [2, 1]);
    assert.deepEqual(fmv.videoInfo, {
        width: 512,
        height: 320,
        frameRate: 30000 / 1001,
        fieldOrder: "progressive",
        sampleAspect: [7, 6],
        displayAspect: [28, 15],
    });
    assert.equal(new TextDecoder().decode(fmv.wav.subarray(0, 4)), "RIFF");
    assert.equal(fmv.wav.length, 44 + 56 * 2 * 2);
    const wav = new DataView(fmv.wav.buffer, fmv.wav.byteOffset, fmv.wav.byteLength);
    assert.notEqual(wav.getInt16(44 + (28 * 2) * 2, true), 0,
                    "filter history must persist into the second channel block");
});

test("fmv: top- and bottom-field metadata come from MPEG extensions", () => {
    assert.equal(demuxFmv(buildFmv({ progressive: false }).bytes)
        .videoInfo.fieldOrder, "tt");
    assert.equal(demuxFmv(buildFmv({ progressive: false, topFieldFirst: false }).bytes)
        .videoInfo.fieldOrder, "bb");
});

test("fmv: MPEG seek index anchors every sequence/GOP/I-picture", () => {
    const start = (code, payload = []) => bytes(0, 0, 1, code, ...payload);
    const sequence0 = start(0xb3, [1]);
    const gop0 = start(0xb8);
    const i0 = start(0x00, [0, 0x08]);
    const p0 = start(0x00, [0, 0x10]);
    const b0 = start(0x00, [0, 0x18]);
    const sequence1 = start(0xb3, [2]);
    const gop1 = start(0xb8);
    const i1 = start(0x00, [0, 0x08]);
    const stream = Uint8Array.from([
        ...sequence0, ...gop0, ...i0, ...p0, ...b0,
        ...sequence1, ...gop1, ...i1, ...start(0xb7),
    ]);
    assert.deepEqual(indexMpeg2SeekPoints(stream, "fixture"), {
        frames: 4,
        points: [
            { offset: 0, frame: 0 },
            {
                offset: sequence0.length + gop0.length + i0.length
                    + p0.length + b0.length,
                frame: 3,
            },
        ],
    });
    assert.throws(
        () => indexMpeg2SeekPoints(
            Uint8Array.from([...gop0, ...i0, ...start(0xb7)]), "fixture"),
        /missing initial sequence\/GOP\/I-picture seek anchor/,
    );
});

test("fmv: malformed offsets, padding and truncation fail hard", () => {
    const original = buildFmv().bytes;
    const secondGroup = new TextEncoder().encode("GroupOfDataInfo\0");
    let first = -1;
    let second = -1;
    for (let i = 0; i <= original.length - secondGroup.length; i++) {
        if (secondGroup.every((value, j) => original[i + j] === value)) {
            if (first < 0) first = i;
            else { second = i; break; }
        }
    }
    assert.ok(first >= 0 && second >= 0);
    const nonzeroPadding = original.slice();
    nonzeroPadding[second - 33] = 1;
    assert.throws(
        () => demuxFmv(nonzeroPadding),
        error => error instanceof FmvFormatError
            && /audio-gap padding/.test(error.message),
    );
    assert.throws(() => demuxFmv(original.subarray(0, second + 10)),
                  /range|truncated|missing following/);
});

test("fmv subtitles: strict UTF-8, timing bounds, spacer removal, SRT and VTT", () => {
    const fixture = buildFmvSubtitles();
    const cues = parseFmvSubtitles(fixture.bin, fixture.sbt, "fixture");
    assert.deepEqual(cues.map(cue => cue.text), ["Hello", "Top\nBottom"]);
    assert.equal(new TextDecoder().decode(subtitlesToSrt(cues)),
        "1\n00:00:00,500 --> 00:00:01,000\nHello\n\n"
        + "2\n00:00:01,250 --> 00:00:02,000\nTop\nBottom\n");
    assert.equal(new TextDecoder().decode(subtitlesToVtt(cues)),
        "WEBVTT\n\n1\n00:00:00.500 --> 00:00:01.000\nHello\n\n"
        + "2\n00:00:01.250 --> 00:00:02.000\nTop\nBottom\n");

    const badUtf8 = fixture.bin.slice();
    badUtf8[fixture.textOffset] = 0xff;
    assert.throws(
        () => parseFmvSubtitles(badUtf8, fixture.sbt),
        error => error instanceof FmvFormatError
            && /strict UTF-8/.test(error.message),
    );
    const badTiming = fixture.sbt.slice();
    new DataView(badTiming.buffer).setFloat32(0x2c, 2.5, true);
    assert.throws(() => parseFmvSubtitles(fixture.bin, badTiming), /invalid range/);
    const badOffset = fixture.bin.slice();
    new DataView(badOffset.buffer).setUint32(0x20, 0xfffffff0, true);
    assert.throws(() => parseFmvSubtitles(badOffset, fixture.sbt), /section order/);
});

/* ---- sz ----------------------------------------------------------------- */

test("sz: round-trip, declared-size and Adler-32 checks", async () => {
    const payload = enc.encode("x".repeat(5000) + "tail");
    const sz = await buildSz(payload);
    assert.deepEqual(await inflateSz(sz), payload);

    const lying = sz.slice();
    lying[0] ^= 1;                                   // declared size off by one
    await assert.rejects(inflateSz(lying), /declared/);

    const corrupt = sz.slice();
    corrupt[corrupt.length - 1] ^= 1;                // break the Adler-32 trailer
    await assert.rejects(inflateSz(corrupt));
});

/* ---- pck ---------------------------------------------------------------- */

test("pck: members, attrs, naming rules", () => {
    const pck = buildPck([
        { name: "hero", attrs: "i3r static prio=1000", data: bytes(1) },
        { name: "hero", attrs: "i3r static prio=1000", data: bytes(2) },
        { name: "/z_esc", attrs: "tm2", data: bytes(3) },
        { name: "cfg.exdb", attrs: "exdb", data: bytes(4, 5) },
        { name: "odd", attrs: "", data: bytes(6) },
    ]);
    const m = unpackPck(pck);
    assert.equal(m.length, 5);
    assert.equal(m[0].name, "hero");
    assert.equal(m[2].name, "/z_esc");
    assert.deepEqual(memberBytes(pck, m[3]), bytes(4, 5));

    assert.equal(typeOf(m[0].attrs), "i3d");          // i3r -> i3d rename
    assert.equal(typeOf("i3c_s geom=x"), "i3c");
    assert.equal(typeOf("tm2"), "tm2");
    assert.equal(typeOf(""), "bin");
    assert.deepEqual(attrsOf("i3r static prio=1000 parent=jnt_armL1"),
                     { static: true, prio: "1000", parent: "jnt_armL1" });

    assert.equal(safeMember("/z_esc"), "z_esc");
    assert.equal(safeMember("a\\b/c"), "c");
    assert.equal(safeMember("///"), "_unnamed");

    assert.deepEqual(pckFileNames(m),
                     ["hero.i3d", "hero.001.i3d", "z_esc.tm2",
                      "cfg.exdb.exdb", "odd.bin"]);

    assert.equal(unpackPck(bytes(1, 2, 3, 4)), null);  // not a PCK
});

/* ---- I3D models, animation, collision ---------------------------------- */

test("models: every I3D family member is catalogued with a stable source id", async () => {
    const pck = buildPck([
        { name: "hero", attrs: "i3r", data: buildEmptyI3d() },
        { name: "run", attrs: "i3m", data: enc.encode("I3D_I3M\0payload") },
        { name: "world", attrs: "i3c_s", data: enc.encode("I3D_I3C\0payload") },
        { name: "diffuse", attrs: "tm2", data: enc.encode("TIM2payload") },
    ]);
    const vfi = await Vfi.open(new BytesSource(buildVfi([
        { path: "stage/test/bg.pck", data: pck },
    ])));
    assert.equal(locateModelContainers(vfi).length, 1);
    const assets = await scanModelAssets(vfi);
    assert.deepEqual(assets.map(asset => [
        asset.kind, asset.fileName, asset.memberIndex, asset.sourcePath,
    ]), [
        ["model", "hero.i3d", 0, "stage/test/bg.pck"],
        ["animation", "run.i3m", 1, "stage/test/bg.pck"],
        ["collision", "world.i3c", 2, "stage/test/bg.pck"],
    ]);
    assert.deepEqual(assets[0].boneNames, []);
    assert.equal(new Set(assets.map(asset => asset.id)).size, 3);
});

test("i3d: authored normals follow split position and UV vertices", () => {
    const model = decodeI3dModel(buildLitI3d(), "lit.i3d");
    assert.equal(model.meshes.length, 1);
    assert.deepEqual([...model.meshes[0].positions], [0, 0, 0, 1, 0, 0, 0, 1, 0]);
    assert.deepEqual([...model.meshes[0].normals], [0, 1, 0, 1, 0, 0, 0, 0, 1]);
    assert.deepEqual([...model.meshes[0].indices], [0, 1, 2]);
});

test("i3m: named quaternion keys decode for realtime skeletal playback", () => {
    const data = new Uint8Array(0x60);
    const view = new DataView(data.buffer);
    data.set(enc.encode("I3D_I3M\0"), 0);
    view.setUint32(8, 0x00020001, true);
    view.setUint32(0x0c, data.length, true);
    view.setUint16(0x10, 7, true);
    view.setUint16(0x12, 4, true);
    view.setUint16(0x14, 1, true);
    view.setUint16(0x16, 1, true);
    view.setFloat32(0x18, 0, true);
    view.setUint32(0x1c, 0x30, true);
    view.setUint32(0x20, 0x50, true);
    view.setUint32(0x24, 0x58, true);
    view.setUint32(0x28, 0x50, true);
    view.setUint32(0x30, 0x3c, true);
    view.setUint16(0x34, 4, true);
    view.setUint16(0x36, 1, true);
    view.setUint32(0x38, 0x48, true);
    data.set(enc.encode("jnt_root\0"), 0x3c);
    view.setUint16(0x48, 0, true);
    view.setUint16(0x4a, 1, true);
    view.setUint16(0x4c, 0, true);
    view.setUint16(0x4e, 0, true);
    view.setInt16(0x56, 0x7fff, true);
    const animation = decodeI3dAnimation(data);
    assert.equal(animation.tracks[0].name, "jnt_root");
    assert.deepEqual([...animation.tracks[0].times], [0]);
    assert.deepEqual([...animation.tracks[0].rotations], [0, 0, 0, 1]);
});

test("i3c: BVH leaf triangles resolve their shared vertex array", () => {
    const data = new Uint8Array(0xb4);
    const view = new DataView(data.buffer);
    data.set(enc.encode("I3D_I3C\0"), 0);
    view.setUint32(8, 0x00030000, true);
    view.setUint32(0x0c, data.length, true);
    data[0x10] = 1;
    view.setUint16(0x14, 1, true);
    view.setUint16(0x16, 1, true);
    view.setUint32(0x18, 0x20, true);
    view.setUint32(0x1c, 0x2c, true);
    view.setUint16(0x24, 1, true);
    view.setUint32(0x28, 0x40, true);
    view.setUint32(0x2c, 0x30, true);
    data.set(enc.encode("coll_wall\0"), 0x30);
    view.setFloat32(0x40, 0.5, true);
    view.setFloat32(0x44, 0.5, true);
    view.setFloat32(0x48, 0, true);
    view.setFloat32(0x4c, 1, true);
    view.setFloat32(0x50, 0.5, true);
    view.setFloat32(0x54, 0.5, true);
    view.setUint32(0x68, 0xa0, true);
    view.setUint32(0x6c, 0x70, true);
    const vertices = [[0, 0, 0], [1, 0, 0], [0, 1, 0]];
    vertices.forEach((vertex, index) => vertex.forEach((value, axis) =>
        view.setFloat32(0x70 + index * 0x10 + axis * 4, value, true)));
    view.setUint16(0xa0, 0x8001, true);
    view.setUint32(0xa8, 0xac, true);
    view.setUint16(0xac, 0, true);
    view.setUint16(0xae, 1, true);
    view.setUint16(0xb0, 2, true);
    const collision = decodeI3dCollision(data);
    assert.equal(collision.material, "coll_wall");
    assert.deepEqual([...collision.positions], [0, 0, 0, 1, 0, 0, 0, 1, 0]);
    assert.deepEqual([...collision.indices], [0, 1, 2]);
});

/* ---- images ------------------------------------------------------------- */

test("tim2: every picture decodes with PS2 alpha", () => {
    const texture = buildTim2([
        {
            width: 2, height: 1, indices: bytes(0, 1),
            palette: bytes(255, 0, 0, 0x80, 0, 255, 0, 0x40),
        },
        {
            width: 1, height: 1, indices: bytes(1),
            palette: bytes(0, 0, 255, 0x80, 255, 255, 0, 0),
        },
    ]);
    assert.deepEqual(inspectTim2(texture).map(picture =>
        [picture.width, picture.height]), [[2, 1], [1, 1]]);
    assert.deepEqual([...decodeTim2(texture).rgba],
        [255, 0, 0, 255, 0, 255, 0, 127]);
    assert.deepEqual([...decodeTim2(texture, 1).rgba], [255, 255, 0, 0]);
    assert.throws(() => decodeTim2(texture, 2), /does not exist/);
});

test("tim2: declared transparent margins are preserved", () => {
    const texture = buildTim2([{
        width: 4, height: 1, indices: bytes(0, 0, 1, 1),
        palette: bytes(0, 0, 0, 0, 255, 255, 255, 0x80),
    }]);
    const image = decodeTim2(texture);
    assert.equal(image.width, 4);
    assert.equal(image.height, 1);
    assert.deepEqual([image.rgba[3], image.rgba[7],
                      image.rgba[11], image.rgba[15]],
                     [0, 0, 255, 255]);
});

test("tim2: 256-color CSM1 palettes are linearized", () => {
    const palette = new Uint8Array(256 * 4);
    palette.set([255, 0, 0, 0x80], 8 * 4);
    palette.set([0, 255, 0, 0x80], 16 * 4);
    const texture = buildTim2([{
        width: 1, height: 1, indices: bytes(8), palette, clutType: 3,
    }]);
    assert.deepEqual([...decodeTim2(texture).rgba], [0, 255, 0, 255]);
});

test("images: direct and compressed PCK textures scan and re-read", async () => {
    const direct = buildTim2([{
        width: 1, height: 1, indices: bytes(0),
        palette: bytes(10, 20, 30, 0x80),
    }]);
    const packedTexture = buildTim2([{
        width: 2, height: 1, indices: bytes(0, 0),
        palette: bytes(40, 50, 60, 0x80),
    }]);
    const pck = buildPck([
        { name: "sprite", attrs: "tm2 pictname=face", data: packedTexture },
        { name: "material", attrs: "tm2", data: packedTexture },
        { name: "layout", attrs: "uis", data: enc.encode("sprite\0") },
        { name: "model", attrs: "i3r", data: enc.encode("material\0") },
        { name: "not_an_image", attrs: "bin", data: bytes(1, 2, 3) },
    ]);
    const vfi = await Vfi.open(new BytesSource(buildVfi([
        { path: "debug/us/static/direct.tm2", data: direct },
        { path: "debug/us/stage/test/ui.pck.sz", data: await buildSz(pck) },
        { path: "debug/us/readme.bin", data: bytes(9) },
    ])));
    assert.deepEqual(locateImageContainers(vfi).map(entry => entry.name),
                     ["direct.tm2", "ui.pck.sz"]);

    const seen = [];
    const progress = [];
    const containers = [];
    const textures = await scanImageTextures(vfi, {
        progress: (done, total, path) => progress.push([done, total, path]),
        texture: (texture, data) => seen.push([texture.id, data.length]),
        container: (entry, data) => containers.push([entry.path, data.length]),
    });
    assert.equal(textures.length, 3);
    assert.deepEqual(textures.map(texture => texture.fileName),
                     ["direct.tm2", "sprite.tm2", "material.tm2"]);
    assert.deepEqual(textures.map(texture => texture.pictures[0].width), [1, 2, 2]);
    assert.deepEqual(textures.map(texture => [texture.role, texture.roleEvidence]), [
        ["other", "direct"],
        ["sprite", "ui-reference"],
        ["texture", "model-reference"],
    ]);
    assert.equal(seen.length, 3);
    assert.deepEqual(containers.map(([path]) => path),
                     ["debug/us/static/direct.tm2", "debug/us/stage/test/ui.pck.sz"]);
    assert.deepEqual(progress.at(-1), [2, 2, "done"]);
    assert.deepEqual(await readImageTexture(vfi, textures[0]), direct);
    assert.deepEqual(await readImageTexture(vfi, textures[2]), packedTexture);
});

test("images: global references precede conservative UI name fallback", async () => {
    const image = buildTim2([{
        width: 1, height: 1, indices: bytes(0),
        palette: bytes(10, 20, 30, 0x80),
    }]);
    const mixed = buildPck([
        { name: "ui_cross", attrs: "tm2", data: image },
        { name: "model_cross", attrs: "tm2", data: image },
        { name: "ui_guess", attrs: "tm2", data: image },
        { name: "ui_model", attrs: "tm2", data: image },
        { name: "ui_conflict", attrs: "tm2", data: image },
        { name: "local_layout", attrs: "uis", data: enc.encode("none\0") },
        { name: "local_model", attrs: "i3r", data: enc.encode("ui_model\0") },
    ]);
    const references = buildPck([
        { name: "remote_layout", attrs: "uis",
          data: enc.encode("ui_cross\0ui_conflict\0") },
        { name: "remote_model", attrs: "i3r",
          data: enc.encode("model_cross\0ui_conflict\0") },
    ]);
    const vfi = await Vfi.open(new BytesSource(buildVfi([
        { path: "debug/us/mixed.pck.sz", data: await buildSz(mixed) },
        { path: "debug/us/references.pck.sz", data: await buildSz(references) },
    ])));
    const seen = [];
    const textures = await scanImageTextures(vfi, {
        texture: texture => seen.push([
            texture.fileName, texture.role, texture.roleEvidence,
        ]),
    });
    const roles = textures.map(texture => [
        texture.fileName, texture.role, texture.roleEvidence,
    ]);
    assert.deepEqual(roles, [
        ["ui_cross.tm2", "sprite", "ui-global-reference"],
        ["model_cross.tm2", "texture", "model-global-reference"],
        ["ui_guess.tm2", "sprite", "ui-name-prefix"],
        ["ui_model.tm2", "texture", "model-reference"],
        ["ui_conflict.tm2", "other", "unclassified"],
    ]);
    assert.deepEqual(seen.toSorted(([left], [right]) => left.localeCompare(right)),
                     roles.toSorted(([left], [right]) => left.localeCompare(right)));
});


/* ---- exdb --------------------------------------------------------------- */

const BGM_FIELDS = [
    { type: "s", offset: 0, name: "name" },
    { type: "s", offset: 32, name: "midi" },
    { type: "s", offset: 64, name: "vh" },
    { type: "s", offset: 96, name: "vb" },
    { type: "s", offset: 128, name: "reverb" },
    { type: "f", offset: 160, name: "volume_scale" },
];

test("exdb: schema, spans, f32 promotion, dup field names", () => {
    const db = parseExdb(buildExdb("Mixed",
        [{ type: "s", offset: 0, name: "tag" },
         { type: "i", offset: 32, name: "prio" },
         { type: "f", offset: 36, name: "gain" },
         { type: "i", offset: 40, name: "prio" }],
        48,
        [["alpha", -3, 0.42, 7], ["beta", 100, 1.5, 8]]));
    assert.equal(db.name, "Mixed");
    assert.deepEqual(db.fields.map(f => f.name), ["tag", "prio", "gain", "prio.1"]);
    assert.deepEqual(db.records[0],
                     { tag: "alpha", prio: -3,
                       gain: Math.fround(0.42), "prio.1": 7 });
    assert.equal(db.records[1].prio, 100);
    /* the f32 0.42 must surface as its exact double promotion */
    assert.equal(db.records[0].gain, 0.41999998688697815);

    assert.throws(() => parseExdb(enc.encode("XXX")), /no EDB magic/);
});

test("exdb: BgmDesc view + the bgmplay song table rules", () => {
    const db = parseExdb(buildExdb("BgmDesc", BGM_FIELDS, 176, [
        ["m01", "s_10.mid", "s_10.hd", "s_10.bd", "system", 0.42],
        ["m02", "s_10.mid", "s_10.hd", "s_10.bd", "system", 0.42],  // dup mid
        ["m03", "s_9.mid", "s_9.hd", "s_9.bd", "system", 1.2],      // vol cap
        ["m04", "b_1_or.mid", "b_1_white_brass.hd", "b_1_white_brass.bd",
         "hall", 0.005],                                            // orphan + <1 -> 44
        ["m05", "nomid", "x.hd", "x.bd", "system", 0.5],            // not a .mid
        ["m06", "p_2.mid", "p_2.hd", "p_2.bd", "system", 0.66],
    ]));
    const recs = bgmDescRecords(db);
    assert.equal(recs.length, 6);
    assert.equal(recs[3].reverb, "hall");

    const songs = bgmSongTable(db);
    assert.deepEqual(songs.map(s => s.name), ["b_1_or", "p_2", "s_9", "s_10"]);
    const byName = Object.fromEntries(songs.map(s => [s.name, s]));
    assert.equal(byName.s_10.songvol, Math.trunc(127 * Math.fround(0.42)));
    assert.equal(byName.s_9.songvol, 126);            // capped
    assert.equal(byName.b_1_or.songvol, 44);          // <1 fallback
    assert.equal(byName.b_1_or.hd, "b_1_white_brass.hd");  // orphan pairing

    assert.throws(() => bgmSongTable(parseExdb(buildExdb("Other",
        BGM_FIELDS, 176, []))), /expected schema BgmDesc/);
});

test("natcmp: numeric runs, case folding", () => {
    assert.ok(natcmp("s_9", "s_10") < 0);
    assert.ok(natcmp("p_2", "p_10") < 0);
    assert.ok(natcmp("p_10", "p_1_retake") > 0);
    assert.equal(natcmp("A_1", "a_1"), 0);
    assert.ok(natcmp("a", "ab") < 0);
    assert.equal(["s_10", "b_8", "s_9", "b_10"].sort(natcmp).join(","),
                 "b_8,b_10,s_9,s_10");
});

/* ---- sniff -------------------------------------------------------------- */

test("sniff: order-sensitive magics", () => {
    assert.equal(sniff(enc.encode("MThd....")), "midi");
    assert.equal(sniff(enc.encode("I3D_BIN\0")), "model");
    assert.equal(sniff(enc.encode("I3D_I3M\0")), "anim");
    assert.equal(sniff(enc.encode("I3D_XYZ\0")), "i3d_unknown");
    assert.equal(sniff(enc.encode("PCK\0....")), "pck");
    assert.equal(sniff(enc.encode("junk")), "?");
    assert.equal(sniff(bytes()), "?");
});

/* ---- iso9660 ------------------------------------------------------------ */

test("iso9660: PVD, root walk, windowing, SYSTEM.CNF serial", async () => {
    const cnf = enc.encode("BOOT2 = cdrom0:\\SCUS_975.01;1\r\nVER = 1.00\r\n");
    const payload = enc.encode("hello disc");
    const iso = await Iso9660.open(new BytesSource(buildIso([
        { name: "SYSTEM.CNF", data: cnf },
        { name: "PAYLOAD.BIN", data: payload },
    ])));
    assert.equal(iso.volumeId, "TESTDISC");
    const names = (await iso.readDir()).map(e => e.name);
    assert.deepEqual(names, ["SYSTEM.CNF", "PAYLOAD.BIN"]);

    const found = await iso.findRoot("payload.bin");   // case-insensitive
    assert.ok(found);
    assert.deepEqual(await iso.window(found).read(0, found.size), payload);
    /* window clamps at the extent end */
    assert.equal((await iso.window(found).read(4, 9999)).length,
                 payload.length - 4);

    const text = new TextDecoder().decode(await iso.window(
        (await iso.findRoot("SYSTEM.CNF"))).read(0, cnf.length));
    assert.equal(systemCnfSerial(text), "SCUS_975.01");
    assert.equal(systemCnfSerial("VER = 1.00"), null);

    await assert.rejects(Iso9660.open(new BytesSource(new Uint8Array(40 * 2048))),
                         /primary volume descriptor/);
});

/* ---- end-to-end --------------------------------------------------------- */

test("openDisc: full chain over a synthetic AE3-shaped disc", async () => {
    const bgmDesc = buildExdb("BgmDesc", BGM_FIELDS, 176, [
        ["m01", "s_9.mid", "s_9.hd", "s_9.bd", "system", 0.42],
        ["m02", "b_8.mid", "b_8.hd", "b_8.bd", "system", 0.9],
    ]);
    const exdbSoundPck = buildPck([
        { name: "other.exdb", attrs: "exdb", data: buildExdb("SeDesc",
            [{ type: "s", offset: 0, name: "name" }], 32, [["se"]]) },
        { name: "bgm_desc.exdb", attrs: "exdb", data: bgmDesc },
    ]);
    const mkwave = tag => enc.encode(`SShd ${tag}`);
    const dataBin = buildVfi([
        { path: "irx/3.0/sg2iopm1.irx", data: enc.encode("\x7fELF sg2") },
        { path: "irx/3.0/libsd.irx", data: enc.encode("\x7fELF sd") },
        { path: "debug/us/sound/bgm/s_9.hd", data: mkwave("s9h") },
        { path: "debug/us/sound/bgm/s_9.bd", data: mkwave("s9b") },
        { path: "debug/us/sound/bgm/s_9.mid", data: enc.encode("MThd s9") },
        { path: "debug/us/sound/bgm/b_8.hd", data: mkwave("b8h") },
        { path: "debug/us/sound/bgm/b_8.bd", data: mkwave("b8b") },
        { path: "debug/us/sound/bgm/b_8.mid", data: enc.encode("MThd b8") },
        { path: "debug/us/static/exdb_sound.pck.sz", data: await buildSz(exdbSoundPck) },
        { path: "debug/us/startup/exdb_common.pck.sz",
          data: await buildSz(buildPck([])) },
    ]);
    const iso = buildIso([
        { name: "SYSTEM.CNF", data: enc.encode("BOOT2 = cdrom0:\\SCUS_975.01;1\r\n") },
        { name: "DATA.BIN", data: dataBin },
    ]);

    const disc = await openDisc(new BytesSource(iso));
    assert.equal(disc.serial, "SCUS_975.01");
    assert.equal(disc.assets.bgmDir, "debug/us/sound/bgm");
    assert.equal(disc.assets.hd.length, 2);
    assert.equal(disc.assets.mid.length, 2);
    assert.ok(disc.assets.sg2iopm1);
    assert.ok(disc.assets.libsd);
    assert.ok(disc.assets.soundConfigPck);
    assert.deepEqual(disc.songs.map(s => s.name), ["b_8", "s_9"]);
    assert.equal(disc.songs[1].songvol, Math.trunc(127 * Math.fround(0.42)));
    assert.deepEqual(await disc.vfi.read(disc.assets.libsd),
                     enc.encode("\x7fELF sd"));
    assert.match(disc.cacheKey, /^SCUS_975\.01-\d+-[0-9a-f]{16}$/);

    /* same image -> same key; different table -> different key */
    const again = await openDisc(new BytesSource(iso));
    assert.equal(again.cacheKey, disc.cacheKey);

    await assert.rejects(
        openDisc(new BytesSource(buildIso([{ name: "OTHER.BIN", data: bytes(1) }]))),
        /DATA\.BIN not found/);
});

/* fixture self-check so compression failures are attributed to the builder */
test("fixtures: buildSz output ends in the payload's Adler-32 (BE)", async () => {
    const data = enc.encode("abcabcabc".repeat(100));
    const sz = await buildSz(data);
    let a = 1, b = 0;                                 // reference Adler-32
    for (const byte of data) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const adler = (b << 16 | a) >>> 0;
    const tail = sz.subarray(sz.length - 4);
    assert.equal((tail[0] << 24 | tail[1] << 16 | tail[2] << 8 | tail[3]) >>> 0,
                 adler);
});
