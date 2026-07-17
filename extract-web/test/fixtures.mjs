/* Synthetic fixture builders: a miniature ISO9660 / VFI / .sz / PCK / EDB
 * constructed in memory at test time -- no game data, no committed binaries.
 * Layouts mirror the real containers as probed from the US disc (see
 * docs/formats/DATA_BIN.md), including the quirks: a folder record at region
 * offset 0 that no file can reference (parent_off 0 means root), PCK member
 * names with a leading '/', duplicate member names, 'U' padding in EDB. */

const enc = new TextEncoder();

class Builder {
    constructor() { this.parts = []; this.length = 0; }
    bytes(b) { this.parts.push(b); this.length += b.length; return this; }
    str(s) { return this.bytes(enc.encode(s)); }
    u8(v) { return this.bytes(Uint8Array.of(v & 0xff)); }
    u16(v) { return this.bytes(Uint8Array.of(v & 0xff, (v >> 8) & 0xff)); }
    u32(v) {
        return this.bytes(Uint8Array.of(v & 0xff, (v >> 8) & 0xff,
                                        (v >> 16) & 0xff, (v >> 24) & 0xff));
    }
    f32(v) {
        const b = new Uint8Array(4);
        new DataView(b.buffer).setFloat32(0, v, true);
        return this.bytes(b);
    }
    pad(n, fill = 0) { return this.bytes(new Uint8Array(n).fill(fill)); }
    padTo(off, fill = 0) {
        if (this.length > off) throw new Error(`overrun: at ${this.length}, want ${off}`);
        return this.pad(off - this.length, fill);
    }
    build() {
        const out = new Uint8Array(this.length);
        let o = 0;
        for (const p of this.parts) { out.set(p, o); o += p.length; }
        return out;
    }
}

export async function deflateZlib(data) {
    const stream = new Blob([data]).stream()
        .pipeThrough(new CompressionStream("deflate"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
}

/** [u32 decompressed_size][raw deflate][Adler-32 BE] -- a zlib stream minus
 *  its 2-byte header, exactly as the real corpus stores it (1706/1706). */
export async function buildSz(payload) {
    const b = new Builder();
    b.u32(payload.length).bytes((await deflateZlib(payload)).subarray(2));
    return b.build();
}

/** members: [{name, attrs, data}] -- string pool then 16-byte entries then blobs. */
export function buildPck(members) {
    const pool = new Builder();
    const poolBase = 12;
    const offs = members.map(m => {
        const nameOff = poolBase + pool.length;
        pool.str(m.name).u8(0);
        const attrOff = poolBase + pool.length;
        pool.str(m.attrs).u8(0);
        return { nameOff, attrOff };
    });
    const infoOff = poolBase + pool.length;
    let dataOff = infoOff + members.length * 16;
    const b = new Builder();
    b.str("PCK").u8(0).u32(infoOff).u32(members.length).bytes(pool.build());
    for (let i = 0; i < members.length; i++) {
        b.u32(offs[i].nameOff).u32(offs[i].attrOff)
         .u32(dataOff).u32(members[i].data.length);
        dataOff += members[i].data.length;
    }
    for (const m of members) b.bytes(m.data);
    return b.build();
}

/** Self-describing EDB table. fields: [{type:'s'|'f'|'i', offset, name}];
 *  records: array of arrays of values in field order. */
export function buildExdb(name, fields, sizest, records) {
    const b = new Builder();
    b.str("EDB\n");
    b.str(`stn:${name}:${fields.length}:${records.length}\n`);
    for (const f of fields) b.str(`${f.type}:${f.offset}:${f.name}\n`);
    b.str(`sizest:${sizest}\n`);
    const base = Math.ceil((b.length + 16) / 16) * 16;   // room for the b: line
    b.str(`b:${base}\n`);
    b.padTo(base, 0x55);                                 // 'U' padding
    for (const rec of records) {
        const r = new Builder();
        fields.forEach((f, i) => {
            r.padTo(f.offset);
            if (f.type === "s") r.str(String(rec[i])).u8(0);
            else if (f.type === "f") r.f32(rec[i]);
            else r.u32(rec[i] | 0);
        });
        r.padTo(sizest);
        b.bytes(r.build());
    }
    return b.build();
}

const VFI_SECTOR = 0x800;

/** files: [{path, data}]. Builds header + entry table + folder region with
 *  single-component chained folder records, data at DATA_OFF sectors. */
export function buildVfi(files) {
    /* folder region: dummy record at offset 0 (unreachable: parent_off 0 in a
     * file entry means root), then one record per unique directory */
    const fb = new Builder();
    fb.u16(8).u16(0).u16(0).u16(0);                      // 8-byte dummy at 0
    const folderOff = new Map();                          // dir path -> region offset
    const addFolder = (dir) => {
        if (folderOff.has(dir)) return folderOff.get(dir);
        const slash = dir.lastIndexOf("/");
        const parent = slash >= 0 ? addFolder(dir.slice(0, slash)) : 0;
        const comp = slash >= 0 ? dir.slice(slash + 1) : dir;
        const off = fb.length;
        const size = 8 + comp.length + 1;
        fb.u16(size).u16(0).u16(parent).u16(0).str(comp).u8(0);
        folderOff.set(dir, off);
        return off;
    };
    const split = files.map(f => {
        const slash = f.path.lastIndexOf("/");
        return {
            dir: slash >= 0 ? f.path.slice(0, slash) : null,
            name: slash >= 0 ? f.path.slice(slash + 1) : f.path,
            data: f.data,
        };
    });
    for (const f of split) if (f.dir !== null) addFolder(f.dir);
    const folderRegion = fb.build();

    /* entry table at 0x20 (no name-hash index; the parser never needs it) */
    const infoOff = 0x20;
    const eb = new Builder();
    const entrySizes = split.map(f => 12 + f.name.length + 1);
    const tableEnd = infoOff + entrySizes.reduce((a, x) => a + x, 0)
                   + folderRegion.length;
    const dataOffSectors = Math.ceil(tableEnd / VFI_SECTOR);
    let sector = dataOffSectors;
    for (const f of split) {
        eb.u16(12 + f.name.length + 1)
          .u16(f.dir !== null ? folderOff.get(f.dir) : 0)
          .u32(sector).u32(f.data.length)
          .str(f.name).u8(0);
        sector += Math.max(1, Math.ceil(f.data.length / VFI_SECTOR));
    }
    const entryTable = eb.build();

    const b = new Builder();
    b.u32(0x00494656).u32(1).u32(dataOffSectors).u32(0);
    b.u16(split.length).u16(folderOff.size + 1);
    b.u32(infoOff).u32(infoOff + entryTable.length)       // +0x18 "dummy" = folders_off
     .u32(infoOff + entryTable.length + folderRegion.length);  // INFO_END
    b.bytes(entryTable).bytes(folderRegion);
    sector = dataOffSectors;
    for (const f of split) {
        b.padTo(sector * VFI_SECTOR);
        b.bytes(f.data);
        sector += Math.max(1, Math.ceil(f.data.length / VFI_SECTOR));
    }
    return b.build();
}

const ISO_SECTOR = 2048;

function dirent(name, lba, size, isDir) {
    const nameBytes = typeof name === "number" ? Uint8Array.of(name) : enc.encode(name);
    let len = 33 + nameBytes.length;
    if (len % 2) len++;                                   // records are even-sized
    const b = new Builder();
    b.u8(len).u8(0);
    b.u32(lba);                                           // LE half
    const be = new Builder();                             // BE half
    be.u8((lba >> 24) & 0xff).u8((lba >> 16) & 0xff).u8((lba >> 8) & 0xff).u8(lba & 0xff);
    b.bytes(be.build());
    b.u32(size);
    const sbe = new Builder();
    sbe.u8((size >> 24) & 0xff).u8((size >> 16) & 0xff).u8((size >> 8) & 0xff).u8(size & 0xff);
    b.bytes(sbe.build());
    b.pad(7);                                             // recording date
    b.u8(isDir ? 2 : 0);                                  // flags
    b.pad(6);                                             // unit/gap/volseq
    b.u8(nameBytes.length).bytes(nameBytes);
    b.padTo(len);
    return b.build();
}

/** files: [{name, data}] all in the root directory. Returns the full image. */
export function buildIso(files, volumeId = "TESTDISC") {
    const rootLba = 18;
    let lba = 19;
    const placed = files.map(f => {
        const at = lba;
        lba += Math.max(1, Math.ceil(f.data.length / ISO_SECTOR));
        return { ...f, lba: at };
    });

    const rd = new Builder();
    rd.bytes(dirent(0, rootLba, 0, true)).bytes(dirent(1, rootLba, 0, true));
    for (const f of placed) rd.bytes(dirent(`${f.name};1`, f.lba, f.data.length, false));
    const rootDir = rd.build();
    if (rootDir.length > ISO_SECTOR) throw new Error("root dir > 1 sector");

    const b = new Builder();
    b.padTo(16 * ISO_SECTOR);
    /* PVD */
    b.u8(1).str("CD001").u8(1).pad(1);
    b.str("SYSTEM_ID".padEnd(32));
    b.str(volumeId.padEnd(32));
    b.padTo(16 * ISO_SECTOR + 128);
    b.u16(ISO_SECTOR).u8(0x08).u8(0x00);                  // logical block size LE+BE
    b.padTo(16 * ISO_SECTOR + 156);
    b.bytes(dirent(0, rootLba, rootDir.length, true));    // root record (34 B)
    b.padTo(17 * ISO_SECTOR);
    b.u8(255).str("CD001").u8(1);                         // terminator
    b.padTo(rootLba * ISO_SECTOR);
    b.bytes(rootDir);
    for (const f of placed) {
        b.padTo(f.lba * ISO_SECTOR);
        b.bytes(f.data);
    }
    b.padTo(lba * ISO_SECTOR);
    return b.build();
}
