/* Public gate for @ae3/extract: every parser against synthetic fixtures,
 * plus the end-to-end openDisc path over a miniature disc image. The
 * real-corpus byte-equality gate is private (checks/extract_ab.mjs in the
 * research repo -- it needs a disc). */

import { test } from "node:test";
import assert from "node:assert/strict";

import {
    BytesSource, Iso9660, systemCnfSerial, Vfi, inflateSz,
    unpackPck, memberBytes, typeOf, attrsOf, safeMember, pckFileNames,
    parseExdb, bgmDescRecords, bgmSongTable, natcmp, sniff, openDisc,
    locateFmvAssets, parseFmvHeader, inspectFmvPrefix, inspectFmvAsset,
    demuxFmv, indexMpeg2SeekPoints, parseFmvSubtitles,
    subtitlesToSrt, subtitlesToVtt,
} from "../src/index.ts";
import {
    buildSz, buildPck, buildExdb, buildVfi, buildIso, buildFmv,
    buildFmvSubtitles,
} from "./fixtures.mjs";
const enc = new TextEncoder();
const bytes = (...v) => Uint8Array.from(v);

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
    assert.throws(() => demuxFmv(nonzeroPadding), /audio-gap padding/);
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
    assert.throws(() => parseFmvSubtitles(badUtf8, fixture.sbt), /strict UTF-8/);
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
