import JSZip from "jszip";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { loadRelease } from "./release-loader";

type Payload = Record<string, Uint8Array>;
const encoder = new TextEncoder();
const digest = (bytes: Uint8Array) => createHash("sha256").update(bytes).digest("hex");
const json = (value: unknown) => encoder.encode(JSON.stringify(value));
const readJSON = (bytes: Uint8Array) => JSON.parse(new TextDecoder().decode(bytes));
const fonts = ["LXGW-v1.522-OFL.txt", "LXGW-OFL.txt", "Montserrat-OFL.txt", "FontAwesome5-LICENSE.txt", "FONT-LICENSES.txt", "font-sources.json"];
const partitions = [
  ["nvs", 1, 2, 0x9000, 0x6000, 0], ["phy_init", 1, 1, 0xf000, 0x1000, 0],
  ["factory", 0, 0, 0x10000, 0x300000, 0], ["imgstore", 1, 0x82, 0x310000, 0x20000, 0],
  ["imgframe", 1, 0x82, 0x330000, 0x26000, 0], ["cardid", 1, 2, 0x356000, 0x4000, 0],
  ["imguser", 1, 0x82, 0x35a000, 0x20000, 0], ["audio", 1, 0x82, 0x37a000, 0x80000, 0],
  ["imgava", 1, 0x82, 0x3fa000, 0x100000, 0], ["petart", 1, 0x40, 0x500000, 0x100000, 2],
] as const;
const segments = [["bootloader", "bootloader.bin", 0], ["partition-table", "partition-table.bin", 0x8000],
  ["factory", "trae_card.bin", 0x10000], ["imgava", "imgava.bin", 0x3fa000], ["petart", "petart.bin", 0x500000]] as const;
function concat(...arrays: Uint8Array[]): Uint8Array {
  const result = new Uint8Array(arrays.reduce((n, bytes) => n + bytes.length, 0)); let offset = 0;
  for (const bytes of arrays) { result.set(bytes, offset); offset += bytes.length; } return result;
}
function crc32(bytes: Uint8Array): number {
  let crc = 0xffffffff;
  for (const byte of bytes) { crc ^= byte; for (let i = 0; i < 8; ++i) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1)); }
  return (~crc) >>> 0;
}
function partitionTable(companion = false): Uint8Array {
  const data = new Uint8Array(0xc00).fill(0xff), view = new DataView(data.buffer);
  const table = companion ? [...partitions, ["companion", 1, 0x41, 0x600000, 0x200000, 2] as const] : partitions;
  table.forEach(([name, type, subtype, offset, size, flags], index) => {
    const pos = index * 32;
    view.setUint16(pos, 0x50aa, true); data[pos + 2] = type; data[pos + 3] = subtype;
    view.setUint32(pos + 4, offset, true); view.setUint32(pos + 8, size, true);
    data.fill(0, pos + 12, pos + 28); data.set(encoder.encode(name), pos + 12); view.setUint32(pos + 28, flags, true);
  }); return data;
}
function petPack(): { data: Uint8Array; catalog: unknown } {
  const symbol = "fixture_pet"; let id = 2166136261;
  for (const byte of encoder.encode(symbol)) id = Math.imul(id ^ byte, 16777619) >>> 0;
  const sprite = new Uint8Array([80, 83, 82, 49, 1, 0, 0, 0, 0, 0, 0, 255, 0, 0, 2, 0, 128, 0]);
  const data = new Uint8Array(88 + sprite.length), v = new DataView(data.buffer);
  data.set(encoder.encode("PTP1")); v.setUint16(4, 1, true); v.setUint16(6, 64, true);
  for (const [pos, value] of [[8, data.length], [12, 1], [16, 24], [20, 64], [24, 88], [64, id], [72, 88], [76, sprite.length], [80, crc32(sprite)]])
    v.setUint32(pos, value, true);
  v.setUint16(68, 1, true); v.setUint16(70, 1, true); data.set(sprite, 88);
  v.setUint32(28, crc32(data.subarray(64, 88)), true); v.setUint32(32, crc32(sprite), true);
  return { data, catalog: {format: "PTP1", version: 1, offset: 0x500000, capacity: 0x100000, bytes: data.length,
    count: 1, sha256: digest(data), sprites: [{symbol, asset_id: id, width: 1, height: 1, offset: 88,
      bytes: sprite.length, crc32: crc32(sprite), sha256: digest(sprite)}]} };
}
function rehash(payload: Payload): void {
  payload.SHA256SUMS = encoder.encode(Object.keys(payload).filter((name) => name !== "SHA256SUMS").sort()
    .map((name) => `${digest(payload[name])}  ${name}\n`).join(""));
}
function refreshSegments(payload: Payload): void {
  const manifest = readJSON(payload["firmware-manifest.json"]);
  const parts = payload["companion.bin"] ? [...segments, ["companion", "companion.bin", 0x600000] as const] : segments;
  manifest.segments = parts.map(([name, file, offset]) => ({name, file, offset, size: payload[file].length,
    end: offset + payload[file].length, erase_start: offset, erase_end: Math.ceil((offset + payload[file].length) / 4096) * 4096,
    sha256: digest(payload[file])}));
  manifest.standalone_application.sha256 = digest(payload["trae_card.bin"]);
  payload["firmware-manifest.json"] = json(manifest); rehash(payload);
}
function fixture(): Payload {
  const pet = petPack();
  const payload: Payload = {"bootloader.bin": new Uint8Array([0xe9]), "partition-table.bin": partitionTable(),
    "imgava.bin": concat(encoder.encode("AVA1"), new Uint8Array(0x100000 - 4)), "petart.bin": pet.data,
    "petart_catalog.json": json(pet.catalog), "avatar_catalog.json": json({avatars: []}), "MANIFEST.txt": encoder.encode("Five explicit segments")};
  for (const name of fonts) payload[name] = new Uint8Array(readFileSync(new URL(`../../../assets/fonts/${name}`, import.meta.url)));
  payload["trae_card.bin"] = concat(new Uint8Array([0xe9]), payload["FONT-LICENSES.txt"], new Uint8Array([0]),
    encoder.encode(`PETART-SHA256:${digest(pet.data)}\0`));
  payload.flash_args = encoder.encode("--flash_mode dio --flash_freq 80m --flash_size detect\n" + segments.map(([, file, offset]) => `0x${offset.toString(16)} ${file}\n`).join(""));
  payload["flasher_args.json"] = json({flash_files: Object.fromEntries(segments.map(([, file, offset]) => [`0x${offset.toString(16)}`, file]))});
  payload["firmware-manifest.json"] = json({schema_version: 1, package_type: "explicit-flash-segments", status: "intermediate", version: "test",
    protected_ranges: [{name: "cardid", start: 0x356000, end: 0x35a000}],
    standalone_application: {file: "trae_card.bin", offset: 0x10000, complete_flash_image: false,
      requires_matching_pet_art: {file: "petart.bin", offset: 0x500000, sha256: digest(pet.data)}},
    pet_art: {file: "petart.bin", catalog: "petart_catalog.json", format: "PTP1", offset: 0x500000,
      capacity: 0x100000, bytes: pet.data.length, count: 1, sha256: digest(pet.data)},
    font_notices: {file: "FONT-LICENSES.txt", sha256: digest(payload["FONT-LICENSES.txt"]), embedded_in_application: true}});
  refreshSegments(payload); return payload;
}
function companionFixture(version = 2): Payload {
  const payload = fixture(), data = new Uint8Array(0x200000).fill(0xff), v = new DataView(data.buffer);
  data.fill(0, 0, 128); data.set(encoder.encode("CPR1")); v.setUint32(4, version, true);
  let offset = 128;
  const sizes = version === 1 ? [4,4,96*156*3,96*156*3] : [4,4,...Array(12).fill(144*144*3)];
  sizes.forEach((size, i) => {
    v.setUint32(16+i*8, offset, true); v.setUint32(20+i*8, size, true); offset += size;
  });
  v.setUint32(8, offset, true); v.setUint32(12, crc32(data.subarray(16, offset)), true);
  payload["companion.bin"] = data;
  payload["ESP-AUDIO-CODEC-LICENSE.txt"] = new Uint8Array(readFileSync(new URL("../../../assets/licenses/ESP-AUDIO-CODEC-LICENSE.txt", import.meta.url)));
  payload["partition-table.bin"] = partitionTable(true);
  payload["trae_card.bin"] = concat(payload["trae_card.bin"], encoder.encode(`COMPANION-SHA256:${digest(data.subarray(0,offset))}\0`));
  const manifest = readJSON(payload["firmware-manifest.json"]);
  manifest.standalone_application.requires_matching_companion = {file:"companion.bin", offset:0x600000, sha256:digest(data)};
  payload["firmware-manifest.json"] = json(manifest);
  payload.flash_args = concat(payload.flash_args, encoder.encode("0x600000 companion.bin\n"));
  const args = readJSON(payload["flasher_args.json"]); args.flash_files["0x600000"] = "companion.bin";
  payload["flasher_args.json"] = json(args); refreshSegments(payload); return payload;
}
function sharedFixture(): Payload {
  const payload = companionFixture();
  payload["petart.bin"] = new Uint8Array(readFileSync(new URL("../../../assets/petart.bin", import.meta.url)));
  payload["petart_catalog.json"] = new Uint8Array(readFileSync(new URL("../../../assets/petart_catalog.json", import.meta.url)));
  payload["trae_card.bin"] = concat(payload["trae_card.bin"], encoder.encode(`PETART-SHA256:${digest(payload["petart.bin"])}\0`));
  const catalog = readJSON(payload["petart_catalog.json"]), manifest = readJSON(payload["firmware-manifest.json"]);
  manifest.features = {pet: false};
  manifest.shared_game_art = {...manifest.pet_art, count: catalog.count, bytes: catalog.bytes, sha256: catalog.sha256, uses: ["dodge", "home-avatar"]};
  delete manifest.pet_art;
  manifest.standalone_application.requires_matching_shared_game_art = {file: "petart.bin", offset: 0x500000, sha256: catalog.sha256};
  delete manifest.standalone_application.requires_matching_pet_art;
  payload["firmware-manifest.json"] = json(manifest); refreshSegments(payload); return payload;
}
function bodyFontFixture(): Payload {
  const p=sharedFixture(), font=new Uint8Array(86), h=new DataView(font.buffer);
  font.set(encoder.encode("BDF1"));h.setUint32(4,1,true);h.setUint32(8,font.length,true);
  h.setUint32(16,64,true);h.setUint32(20,1,true);h.setUint32(24,68,true);h.setUint32(28,2,true);
  h.setUint32(32,1,true);h.setUint32(36,84,true);h.setUint32(40,1,true);
  font[64]=0xf0;font.fill(255,65,68);h.setUint32(76,16<<20,true);font[80]=font[81]=1;
  h.setUint32(12,crc32(font.subarray(16)),true);
  const meta={format:"BDF1",offset_in_partition:0x10000,bytes:font.length,sha256:digest(font)};
  const catalog=readJSON(p["petart_catalog.json"]), manifest=readJSON(p["firmware-manifest.json"]);
  catalog.body_font=meta;
  p["petart.bin"]=concat(p["petart.bin"],new Uint8Array(0x10000-p["petart.bin"].length).fill(255),font);
  p["petart_catalog.json"]=json(catalog);
  manifest.body_font={...meta,file:"petart.bin",offset:0x510000};
  manifest.shared_game_art.bytes=p["petart.bin"].length;manifest.shared_game_art.sha256=digest(p["petart.bin"]);
  manifest.standalone_application.requires_matching_shared_game_art.sha256=digest(p["petart.bin"]);
  const data=new Uint8Array(0x200000).fill(255), v=new DataView(data.buffer);
  data.fill(0,0,128);data.set(encoder.encode("CPR1"));v.setUint32(4,3,true);
  let end=128;
  [32,4,...Array(12).fill(144*144*3)].forEach((n,i)=>{v.setUint32(16+i*8,end,true);v.setUint32(20+i*8,n,true);end+=n;});
  data.set(Uint8Array.from(meta.sha256.match(/../g)!.map((x:string)=>parseInt(x,16))),128);
  v.setUint32(8,end,true);v.setUint32(12,crc32(data.subarray(16,end)),true);p["companion.bin"]=data;
  manifest.standalone_application.requires_matching_companion.sha256=digest(data);
  p["trae_card.bin"]=concat(p["trae_card.bin"],encoder.encode(`BODYFONT-SHA256:${meta.sha256}\0COMPANION-SHA256:${digest(data.subarray(0,end))}\0`));
  p["firmware-manifest.json"]=json(manifest);refreshSegments(p);return p;
}
async function zipFile(payload: Payload): Promise<File> {
  const zip = new JSZip(); for (const [name, bytes] of Object.entries(payload)) zip.file(name, bytes);
  const bytes = await zip.generateAsync({type: "uint8array"});
  return Object.assign(bytes, {name: "release.zip"}) as unknown as File;
}

describe("release ZIP validation", () => {
  it("loads a complete mapped body font with unchanged partition boundaries",async()=>{
    expect((await loadRelease(await zipFile(bodyFontFixture()))).files.map(f=>f.address)).toEqual([0,0x8000,0x10000,0x3fa000,0x500000,0x600000]);
  });
  it.each(["pixels","padding","truncated","catalog","manifest","missing"])("rejects %s body font damage despite refreshed outer hashes",async(kind)=>{
    const p=bodyFontFixture();
    if(kind==="pixels")p["petart.bin"][0x10040]^=1;
    if(kind==="padding")p["petart.bin"][0xffff]=0;
    if(kind==="truncated")p["petart.bin"]=p["petart.bin"].slice(0,-1);
    if(kind==="catalog"){const c=readJSON(p["petart_catalog.json"]);c.body_font.sha256="0".repeat(64);p["petart_catalog.json"]=json(c);}
    if(kind==="manifest"){const m=readJSON(p["firmware-manifest.json"]);m.body_font.offset=0x500000;p["firmware-manifest.json"]=json(m);}
    if(kind==="missing"){const c=readJSON(p["petart_catalog.json"]);delete c.body_font;p["petart_catalog.json"]=json(c);p["petart.bin"]=p["petart.bin"].slice(0,c.bytes);}
    refreshSegments(p);await expect(loadRelease(await zipFile(p))).rejects.toThrow(/body font|中文字库/);
  });
  it("rejects a companion digest pointing at a different body font",async()=>{
    const p=bodyFontFixture(),d=p["companion.bin"],v=new DataView(d.buffer);d[128]^=1;
    const used=v.getUint32(8,true);v.setUint32(12,crc32(d.subarray(16,used)),true);
    p["trae_card.bin"]=concat(p["trae_card.bin"],encoder.encode(`COMPANION-SHA256:${digest(d.subarray(0,used))}\0`));
    refreshSegments(p);await expect(loadRelease(await zipFile(p))).rejects.toThrow(/body font/);
  });
  it("loads pet-free shared Dodge and home-avatar resources", async () => {
    expect((await loadRelease(await zipFile(sharedFixture()))).files.length).toBe(6);
  });
  it("rejects a pet-free claim with legacy pet metadata", async () => {
    const p = companionFixture(), manifest = readJSON(p["firmware-manifest.json"]);
    manifest.features = {pet: false}; p["firmware-manifest.json"] = json(manifest); rehash(p);
    await expect(loadRelease(await zipFile(p))).rejects.toThrow(/shared|共享/);
  });
  it("rejects pet payloads relabeled as shared game art", async () => {
    const p = companionFixture(), manifest = readJSON(p["firmware-manifest.json"]);
    manifest.features = {pet: false}; manifest.shared_game_art = {...manifest.pet_art, uses: ["dodge", "home-avatar"]}; delete manifest.pet_art;
    manifest.standalone_application.requires_matching_shared_game_art = manifest.standalone_application.requires_matching_pet_art;
    delete manifest.standalone_application.requires_matching_pet_art;
    p["firmware-manifest.json"] = json(manifest); rehash(p);
    await expect(loadRelease(await zipFile(p))).rejects.toThrow(/shared|共享/);
  });
  it("rejects corrupted retained shared sprites despite refreshed outer hashes", async () => {
    const p = sharedFixture(); p["petart.bin"][p["petart.bin"].length-1] ^= 1; refreshSegments(p);
    await expect(loadRelease(await zipFile(p))).rejects.toThrow(/CRC/);
  });
  it.skipIf(!process.env.PASSPORT_RELEASE_ZIP)("loads the supplied real six-segment build", async () => {
    const bytes = new Uint8Array(readFileSync(process.env.PASSPORT_RELEASE_ZIP!));
    const file = Object.assign(bytes, {name: "candidate.zip"}) as unknown as File;
    const release = await loadRelease(file);
    expect(release.files.map(f => f.address)).toEqual([0,0x8000,0x10000,0x3fa000,0x500000,0x600000]);
  });
  it("retains compatibility with the previous static portrait pack", async () => {
    expect((await loadRelease(await zipFile(companionFixture(1)))).files.length).toBe(6);
  });
  it("loads six segments with two animated companion faces", async () => {
    const release = await loadRelease(await zipFile(companionFixture()));
    expect(release.files.map(file => file.address)).toEqual([0,0x8000,0x10000,0x3fa000,0x500000,0x600000]);
  });
  it("rejects corrupt companion even after outer hashes are updated", async () => {
    const p=companionFixture(); p["companion.bin"][140]^=1; refreshSegments(p);
    await expect(loadRelease(await zipFile(p))).rejects.toThrow(/CRC/);
  });
  it("rejects a resource overlap with a valid companion CRC", async () => {
    const p=companionFixture(), data=p["companion.bin"], v=new DataView(data.buffer);
    v.setUint32(32, 128, true); v.setUint32(12,crc32(data.subarray(16,v.getUint32(8,true))),true); refreshSegments(p);
    await expect(loadRelease(await zipFile(p))).rejects.toThrow(/资源范围/);
  });
  it("rejects an app requiring a different companion", async () => {
    const p=companionFixture(), app=p["trae_card.bin"]; app[app.length-2]^=1; refreshSegments(p);
    await expect(loadRelease(await zipFile(p))).rejects.toThrow(/companion 指纹/);
  });
  it("rejects a missing companion required by the six-segment manifest", async () => {
    const p=companionFixture(); delete p["companion.bin"]; rehash(p);
    await expect(loadRelease(await zipFile(p))).rejects.toThrow();
  });

  it("loads a complete hash-bound five-segment release", async () => {
    const release = await loadRelease(await zipFile(fixture()));
    expect(release.files.map((file) => file.address)).toEqual([0, 0x8000, 0x10000, 0x3fa000, 0x500000]);
    for (const file of release.files) expect(file.address >= 0x35a000 || file.address + file.data.length <= 0x356000).toBe(true);
  });
  it.each(["firmware-manifest.json", "SHA256SUMS", "petart_catalog.json", "petart.bin"])("rejects missing %s", async (name) => {
    const payload = fixture(); delete payload[name]; await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/缺少/);
  });
  it("rejects five bare BINs without metadata", async () => {
    const payload = fixture(); for (const name of Object.keys(payload)) if (!name.endsWith(".bin")) delete payload[name];
    await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/元数据/);
  });
  it("rejects a pet payload bit flip using SHA256SUMS", async () => {
    const payload = fixture(); payload["petart.bin"][105] ^= 1;
    await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/SHA256/);
  });
  it("rejects bad native CRC after file/manifest hashes are recomputed", async () => {
    const payload = fixture(); payload["petart.bin"][105] ^= 1; refreshSegments(payload);
    await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/CRC/);
  });
  it.each([[0, 2, 0], [0, 3, 0xff], [9, 3, 0x41], [9, 28, 0]])("rejects changed partition type/subtype/flags %j", async (index, field, value) => {
    const payload = fixture(); payload["partition-table.bin"][index * 32 + field] = value; refreshSegments(payload);
    await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/分区表/);
  });
  it("rejects changed protected identity offset", async () => {
    const payload = fixture(); new DataView(payload["partition-table.bin"].buffer).setUint32(5 * 32 + 4, 0x355000, true); refreshSegments(payload);
    await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/cardid/);
  });
  it("rejects a mismatched app even with valid file hashes", async () => {
    const payload = fixture(); const app = payload["trae_card.bin"]; app[app.length - 2] ^= 1; refreshSegments(payload);
    await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/指纹/);
  });
  it("rejects an app erase extent entering preserved data", async () => {
    const payload = fixture(); payload["trae_card.bin"] = new Uint8Array(0x300001); refreshSegments(payload);
    await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/擦除范围/);
  });
  it("rejects a wrong catalog with recomputed file hashes", async () => {
    const payload = fixture(), catalog = readJSON(payload["petart_catalog.json"]); catalog.sprites[0].symbol = "different_pet";
    payload["petart_catalog.json"] = json(catalog); rehash(payload);
    await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/catalog/);
  });
  it("rejects a mismatched text flash address", async () => {
    const payload = fixture(); payload.flash_args = encoder.encode(new TextDecoder().decode(payload.flash_args).replace("0x500000", "0x356000")); rehash(payload);
    await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/flash_args/);
  });
  it("rejects missing embedded font notices", async () => {
    const payload = fixture(); payload["trae_card.bin"][5] ^= 1; refreshSegments(payload);
    await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/字体许可/);
  });
  it("rejects extra files not covered by SHA256SUMS", async () => {
    const payload = fixture(); payload["unexpected.txt"] = encoder.encode("unlisted");
    await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/SHA256SUMS/);
  });
  it("rejects nested ZIP paths", async () => {
    const payload = fixture(); payload["nested/file.txt"] = encoder.encode("nested"); rehash(payload);
    await expect(loadRelease(await zipFile(payload))).rejects.toThrow(/根目录/);
  });
  it("rejects duplicate ZIP names before JSZip can collapse them", async () => {
    const payload = fixture(); payload["MANIFESS.txt"] = payload["MANIFEST.txt"]; rehash(payload);
    const bytes = new Uint8Array(await zipFile(payload) as unknown as Uint8Array);
    const from = encoder.encode("MANIFESS.txt"), to = encoder.encode("MANIFEST.txt");
    let changed = 0;
    for (let pos = 0; pos <= bytes.length - from.length; ++pos) {
      if (from.every((byte, index) => bytes[pos + index] === byte)) { bytes.set(to, pos); ++changed; }
    }
    expect(changed).toBeGreaterThanOrEqual(2);
    await expect(loadRelease(Object.assign(bytes, {name: "duplicate.zip"}) as unknown as File)).rejects.toThrow(/重复/);
  });

});
