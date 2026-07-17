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
} from "../src/index.ts";
import {
    buildSz, buildPck, buildExdb, buildVfi, buildIso,
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
