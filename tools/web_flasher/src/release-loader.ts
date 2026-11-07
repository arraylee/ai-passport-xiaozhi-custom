import JSZip from "jszip";

export interface FirmwareFile { name: string; address: number; data: Uint8Array }
export interface LoadedRelease { name: string; files: FirmwareFile[] }
type ObjectValue = Record<string, unknown>;
const ENCODER = new TextEncoder();
const DECODER = new TextDecoder("utf-8", { fatal: true });
const PARTITIONS = [
  ["nvs", 1, 2, 0x9000, 0x6000, 0], ["phy_init", 1, 1, 0xf000, 0x1000, 0],
  ["factory", 0, 0, 0x10000, 0x300000, 0], ["imgstore", 1, 0x82, 0x310000, 0x20000, 0],
  ["imgframe", 1, 0x82, 0x330000, 0x26000, 0], ["cardid", 1, 2, 0x356000, 0x4000, 0],
  ["imguser", 1, 0x82, 0x35a000, 0x20000, 0], ["audio", 1, 0x82, 0x37a000, 0x80000, 0],
  ["imgava", 1, 0x82, 0x3fa000, 0x100000, 0], ["petart", 1, 0x40, 0x500000, 0x100000, 2],
] as const;
const SEGMENTS = [
  ["bootloader", "bootloader.bin", 0, 0x8000],
  ["partition-table", "partition-table.bin", 0x8000, 0x9000],
  ["factory", "trae_card.bin", 0x10000, 0x310000],
  ["imgava", "imgava.bin", 0x3fa000, 0x4fa000],
  ["petart", "petart.bin", 0x500000, 0x600000],
] as const;
const COMPANION_SEGMENT = ["companion", "companion.bin", 0x600000, 0x800000] as const;
const COMPANION_PARTITION = ["companion", 1, 0x41, 0x600000, 0x200000, 2] as const;
const FONT_LICENSES = ["LXGW-v1.522-OFL.txt", "LXGW-OFL.txt", "Montserrat-OFL.txt", "FontAwesome5-LICENSE.txt"];
const METADATA = ["avatar_catalog.json", "petart_catalog.json", "flash_args", "flasher_args.json",
  "firmware-manifest.json", "MANIFEST.txt", "SHA256SUMS", "FONT-LICENSES.txt", "font-sources.json", ...FONT_LICENSES];

function require(condition: unknown, message: string): asserts condition {
  if (!condition) throw new Error(message);
}
function object(value: unknown, name: string): ObjectValue {
  require(value !== null && typeof value === "object" && !Array.isArray(value), `${name} JSON 结构错误`);
  return value as ObjectValue;
}
function array(value: unknown, name: string): unknown[] {
  require(Array.isArray(value), `${name} 必须是数组`); return value;
}
function view(data: Uint8Array): DataView { return new DataView(data.buffer, data.byteOffset, data.byteLength); }
function ascii(data: Uint8Array, from: number, length: number): string { return DECODER.decode(data.subarray(from, from + length)); }
async function sha256(data: Uint8Array): Promise<string> {
  const hash = await crypto.subtle.digest("SHA-256", new Uint8Array(data));
  return Array.from(new Uint8Array(hash), (byte) => byte.toString(16).padStart(2, "0")).join("");
}
function contains(data: Uint8Array, expected: Uint8Array): boolean {
  if (!expected.length) return false;
  for (let pos = data.indexOf(expected[0]); pos >= 0; pos = data.indexOf(expected[0], pos + 1)) {
    if (pos + expected.length <= data.length && expected.every((byte, i) => byte === data[pos + i])) return true;
  }
  return false;
}
function terminated(data: Uint8Array): Uint8Array { const bytes = new Uint8Array(data.length + 1); bytes.set(data); return bytes; }
function crc32(data: Uint8Array): number {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (~crc) >>> 0;
}
function symbolId(symbol: string): number {
  require(/^[A-Za-z_][A-Za-z0-9_]*$/.test(symbol), "petart symbol 无效");
  let hash = 2166136261;
  for (const byte of ENCODER.encode(symbol)) hash = Math.imul(hash ^ byte, 16777619) >>> 0;
  return hash;
}

/* JSZip collapses duplicate names. Inspect the central directory first so a
 * conflicting duplicate cannot silently replace a manifest or image. Release
 * files are small, flat, single-disk ZIPs; ZIP64 and directory entries are not
 * part of scripts/release.py's delivery format. */
function archiveNames(bytes: Uint8Array): string[] {
  const v = view(bytes);
  let end = -1;
  for (let pos = bytes.length - 22; pos >= Math.max(0, bytes.length - 65557); --pos) {
    if (v.getUint32(pos, true) === 0x06054b50 && pos + 22 + v.getUint16(pos + 20, true) === bytes.length) {
      end = pos; break;
    }
  }
  require(end >= 0, "release ZIP 目录无效");
  const count = v.getUint16(end + 10, true), length = v.getUint32(end + 12, true);
  let pos = v.getUint32(end + 16, true);
  require(!v.getUint16(end + 4, true) && !v.getUint16(end + 6, true) && count > 0 && count !== 0xffff &&
    count === v.getUint16(end + 8, true) && pos + length === end, "release ZIP 必须是普通单卷文件");
  const names: string[] = [];
  for (let i = 0; i < count; ++i) {
    require(pos + 46 <= end && v.getUint32(pos, true) === 0x02014b50, "release ZIP 目录条目无效");
    const nameLength = v.getUint16(pos + 28, true);
    const next = pos + 46 + nameLength + v.getUint16(pos + 30, true) + v.getUint16(pos + 32, true);
    require(next <= end && !(v.getUint16(pos + 8, true) & 1), "release ZIP 目录越界或已加密");
    const name = ascii(bytes, pos + 46, nameLength);
    require(name.length && !name.includes("/") && !name.includes("\\") && name !== "." && name !== ".." &&
      !names.includes(name), `release ZIP 必须是无重复的根目录文件: ${name}`);
    names.push(name); pos = next;
  }
  require(pos === end && !names.includes("merged-firmware.bin"), "release ZIP 不允许跨分区 raw merge");
  return names;
}

function validatePartitions(data: Uint8Array, companion: boolean, themes: boolean): void {
  const partitions = themes ? [...PARTITIONS.slice(0, 8),
    ["imgava", 1, 0x82, 0x3fa000, 0x20000, 0], ["assets", 1, 0x82, 0x41a000, 0xe6000, 0],
    ["petart", 1, 0x40, 0x500000, 0xe9000, 2], ["theme_cache", 1, 0x42, 0x5e9000, 0x17000, 0],
    COMPANION_PARTITION] : companion ? [...PARTITIONS, COMPANION_PARTITION] : PARTITIONS;
  require(data.length === 0xc00, "分区表长度必须是 0xC00");
  const v = view(data);
  partitions.forEach(([name, type, subtype, offset, size, flags], index) => {
    const pos = index * 32;
    const label = data.subarray(pos + 12, pos + 28), nul = label.indexOf(0);
    require(v.getUint16(pos, true) === 0x50aa && DECODER.decode(nul < 0 ? label : label.subarray(0, nul)) === name &&
      data[pos + 2] === type && data[pos + 3] === subtype && v.getUint32(pos + 4, true) === offset &&
      v.getUint32(pos + 8, true) === size && v.getUint32(pos + 28, true) === flags,
      `分区表 ${name} 的 type/subtype/offset/size/flags 不匹配`);
  });
  const magic = v.getUint16(partitions.length * 32, true);
  require(magic === 0xffff || magic === 0xebeb, "分区表含有非预期分区");
}
function validateSprite(data: Uint8Array, width: number, height: number): void {
  require(width >= 1 && width <= 240 && height >= 1 && height <= 320 && data.length >= 8 &&
    ascii(data, 0, 4) === "PSR1" && data[4] >= 1 && data[4] <= 64 && !data[5] && !data[6] && !data[7], "petart PSR1 头/尺寸无效");
  const table = 8 + data[4] * 4, base = table + (height + 1) * 2, v = view(data);
  require(base < data.length && data.length - base <= 65535 && !v.getUint16(table, true) &&
    v.getUint16(table + height * 2, true) === data.length - base, "petart PSR1 行索引无效");
  for (let y = 0; y < height; ++y) {
    let pos = v.getUint16(table + y * 2, true), x = 0;
    const end = v.getUint16(table + (y + 1) * 2, true);
    require(pos < end && end <= data.length - base, "petart PSR1 行越界");
    while (pos < end) {
      const tag = data[base + pos++], count = (tag & 127) + 1, bytes = tag & 128 ? 1 : count;
      require(x + count <= width && pos + bytes <= end, "petart PSR1 像素串越界");
      for (let i = 0; i < bytes; ++i) require(data[base + pos + i] < data[4], "petart PSR1 调色板索引越界");
      pos += bytes; x += count;
    }
    require(x === width, "petart PSR1 行宽错误");
  }
}
async function validatePetPack(data: Uint8Array, catalog: ObjectValue, app: Uint8Array, digest: string, capacity: number): Promise<number> {
  if (catalog.body_font !== undefined) {
    const meta = object(catalog.body_font, "body font catalog"), length = Number(catalog.bytes);
    require(length >= 64 && length <= 0x10000 && data.length > 0x10040 && data.length <= 0x100000 &&
      data.subarray(length,0x10000).every(b => b === 255), "body font bundle bounds/padding");
    const font = data.subarray(0x10000), h = view(font), fontDigest = await sha256(font);
    require(ascii(font,0,4) === "BDF1" && [1,2].includes(h.getUint32(4,true)) && h.getUint32(8,true) === font.length &&
      crc32(font.subarray(16)) === h.getUint32(12,true), "body font header/CRC");
    const maps = h.getUint32(32,true), glyphs = h.getUint32(28,true);
    require(maps >= 1 && maps <= 2 && glyphs >= 2 && glyphs <= 65535 &&
      font.subarray(36+maps*8,h.getUint32(4,true)===2?60:64).every(b => b === 0), "body font cmap count/reserved");
    const entries = [[h.getUint32(16,true),h.getUint32(20,true)], [h.getUint32(24,true),glyphs*8]];
    let total = 0;
    for(let i=0;i<maps;i++){const n=h.getUint32(40+i*8,true);entries.push([h.getUint32(36+i*8,true),n*2]);total+=n;}
    require(total === glyphs-1,"body font glyph count");
    let end = 64;
    for(const [at,n] of entries){require(at === ((end+3)&~3) && n>0 && at+n<=font.length &&
      font.subarray(end,at).every(b=>b===255),"body font section bounds");end=at+n;}
    if (h.getUint32(4,true)===2) {
      const at=h.getUint32(60,true);
      require(at===((end+3)&~3) && at+24<=font.length && ascii(font,at,4)==="KOR1","Korean font bounds");
    } else require(end===font.length,"body font trailing bytes");
    require(meta.format === "BDF1" && meta.offset_in_partition === 0x10000 && meta.bytes === font.length &&
      meta.sha256 === fontDigest && Object.keys(meta).length === 4,"body font catalog mismatch");
    require(contains(app,terminated(ENCODER.encode(`BODYFONT-SHA256:${fontDigest}`))),"应用与 body font 指纹不匹配");
    data=data.subarray(0,length);digest=await sha256(data);
  } else {
    require(!contains(app, ENCODER.encode("BODYFONT-SHA256:")),"应用需要完整的中文字库包");
  }
  require(data.length >= 64 && data.length <= 0x100000 && ascii(data, 0, 4) === "PTP1", "petart.bin 头/长度无效");
  const v = view(data), count = v.getUint32(12, true), base = 64 + count * 24;
  require(v.getUint16(4, true) === 1 && v.getUint16(6, true) === 64 && v.getUint32(8, true) === data.length &&
    count >= 1 && count <= 1024 && v.getUint32(16, true) === 24 && v.getUint32(20, true) === 64 &&
    v.getUint32(24, true) === base && base < data.length && data.subarray(36, 64).every((byte) => !byte), "petart.bin 格式/边界无效");
  require(crc32(data.subarray(64, base)) === v.getUint32(28, true) &&
    crc32(data.subarray(base)) === v.getUint32(32, true), "petart.bin CRC 错误");
  const records = array(catalog.sprites, "petart_catalog sprites");
  require(catalog.format === "PTP1" && catalog.version === 1 && catalog.offset === 0x500000 && catalog.capacity === capacity &&
    catalog.bytes === data.length && catalog.count === count && catalog.sha256 === digest && records.length === count,
    "petart catalog 与图包不匹配");
  let end = base, lastId = -1;
  const checks: {data: Uint8Array; expected: unknown}[] = [];
  records.forEach((value, index) => {
    const record = object(value, "petart sprite"), pos = 64 + index * 24;
    const id = v.getUint32(pos, true), width = v.getUint16(pos + 4, true), height = v.getUint16(pos + 6, true);
    const offset = v.getUint32(pos + 8, true), length = v.getUint32(pos + 12, true), crc = v.getUint32(pos + 16, true);
    require(id > lastId && !v.getUint32(pos + 20, true) && offset === end && length > 0 && length <= data.length - offset,
      "petart ID/顺序/边界无效");
    require(record.asset_id === id && record.width === width && record.height === height && record.offset === offset &&
      record.bytes === length && record.crc32 === crc && typeof record.symbol === "string" && symbolId(record.symbol) === id,
      "petart asset catalog 不匹配");
    const raw = data.subarray(offset, offset + length);
    require(crc32(raw) === crc, "petart asset CRC 错误"); validateSprite(raw, width, height);
    checks.push({data: raw, expected: record.sha256});
    end = offset + length; lastId = id;
  });
  require(end === data.length, "petart 含未索引尾部数据");
  await Promise.all(checks.map(async ({data, expected}) => {
    require(await sha256(data) === expected, "petart asset SHA256 错误");
  }));
  require(contains(app, terminated(ENCODER.encode(`PETART-SHA256:${digest}`))), "应用 BIN 与 petart.bin 指纹不匹配");
  return count;
}

export async function loadRelease(file: File): Promise<LoadedRelease> {
  require(file.name.toLowerCase().endsWith(".zip"), "请选择 scripts/release.py 生成的 ZIP");
  const bytes = file instanceof Uint8Array ? file : new Uint8Array(await file.arrayBuffer());
  const names = archiveNames(bytes);
  const hasCompanion = names.includes("companion.bin");
  const segments = hasCompanion ? [...SEGMENTS, COMPANION_SEGMENT] : SEGMENTS;
  for (const name of [...segments.map((item) => item[1]), ...METADATA, ...(hasCompanion ? ["ESP-AUDIO-CODEC-LICENSE.txt"] : [])])
    require(names.includes(name), `release ZIP 缺少元数据或文件: ${name}`);
  const zip = await JSZip.loadAsync(bytes, { checkCRC32: true });
  const payload: Record<string, Uint8Array> = {};
  const hashes: Record<string, string> = {};
  await Promise.all(names.map(async (name) => {
    const entry = zip.file(name); require(entry, `release ZIP 文件无效: ${name}`);
    payload[name] = await entry.async("uint8array"); hashes[name] = await sha256(payload[name]);
  }));
  const sums = new Map<string, string>();
  for (const line of DECODER.decode(payload.SHA256SUMS).trimEnd().split("\n")) {
    const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
    require(match && !sums.has(match[2]), "SHA256SUMS 行无效或重复"); sums.set(match[2], match[1]);
  }
  require(sums.size === names.length - 1, "SHA256SUMS 文件集合错误");
  for (const name of names) if (name !== "SHA256SUMS") require(sums.get(name) === hashes[name], `SHA256SUMS 不匹配: ${name}`);
  const json = (name: string) => object(JSON.parse(DECODER.decode(payload[name])), name);
  const manifest = json("firmware-manifest.json"), entries = array(manifest.segments, "manifest segments");
  require(manifest.package_type === "explicit-flash-segments" && entries.length === segments.length, "manifest 必须声明全部显式段");
  const themes = /^2\.1\.0-xiaozhi\.(\d+)$/.exec(String(manifest.version));
  const themeLayout = !!themes && Number(themes[1]) >= 15;
  const files: FirmwareFile[] = [];
  segments.forEach(([name, filename, address, limit], index) => {
    const data = payload[filename], entry = object(entries[index], "manifest segment");
    const eraseStart = Math.floor(address / 4096) * 4096, eraseEnd = Math.ceil((address + data.length) / 4096) * 4096;
    require(data.length > 0 && eraseStart >= address && eraseEnd <= (themeLayout && name === "petart" ? 0x5e9000 : themeLayout && name === "imgava" ? 0x41a000 : limit), `${filename} 写入/擦除范围超出允许分区`);
    require(entry.name === name && entry.file === filename && entry.offset === address && entry.size === data.length &&
      entry.end === address + data.length && entry.erase_start === eraseStart && entry.erase_end === eraseEnd &&
      entry.sha256 === hashes[filename], `manifest segment 不匹配: ${filename}`);
    files.push({name: filename, address, data});
  });
  validatePartitions(payload["partition-table.bin"], hasCompanion, themeLayout);
  const app = payload["trae_card.bin"], pet = payload["petart.bin"], avatar = payload["imgava.bin"];
  require(payload["bootloader.bin"][0] === 0xe9 && app[0] === 0xe9, "bootloader/app 不是 ESP 镜像");
  require(avatar.length === (themeLayout?0x20000:0x100000) && ascii(avatar, 0, 4) === "AVA1", "imgava.bin 头/长度无效");
  const count = await validatePetPack(pet, json("petart_catalog.json"), app, hashes["petart.bin"], themeLayout ? 0xe9000 : 0x100000);
  const fontMeta = json("petart_catalog.json").body_font;
  if(fontMeta !== undefined){
    const expected = {...object(fontMeta,"body font"),file:"petart.bin",offset:0x510000};
    const actual = object(manifest.body_font,"manifest body font");
    require(Object.keys(actual).length===Object.keys(expected).length &&
      Object.entries(expected).every(([k,v])=>actual[k]===v),"manifest body font 不匹配");
  } else require(manifest.body_font===undefined,"unexpected body font metadata");
  const shared = "shared_game_art" in manifest;
  if (shared) {
    const features = object(manifest.features, "shared game features");
    require(Object.keys(features).length === 1 && features.pet === false && !("pet_art" in manifest), "shared game metadata must declare pet removed");
    const symbols = ["bobo_art_wave", "bobo_art_walk_left", "bobo_art_walk_right", "dodge_art_books", "dodge_art_luggage", "dodge_art_robot", "dodge_art_plant"];
    const records = array(json("petart_catalog.json").sprites, "shared game sprites");
    require(records.length === symbols.length && records.every((r) => symbols.includes(String(object(r, "shared sprite").symbol))), "shared game art must contain only all seven shared sprites");
  } else if (manifest.features !== undefined) {
    require(object(manifest.features, "features").pet !== false, "pet-free firmware requires shared_game_art metadata");
  }
  const petMeta = object(shared ? manifest.shared_game_art : manifest.pet_art, "manifest shared/pet art"), standalone = object(manifest.standalone_application, "standalone app");
  if (shared) require(JSON.stringify(petMeta.uses) === JSON.stringify(["dodge", "home-avatar"]), "shared game art uses mismatch");
  require(petMeta.file === "petart.bin" && petMeta.catalog === "petart_catalog.json" && petMeta.format === "PTP1" &&
    petMeta.offset === 0x500000 && petMeta.capacity === (themeLayout ? 0xe9000 : 0x100000) && petMeta.count === count && petMeta.bytes === pet.length &&
    petMeta.sha256 === hashes["petart.bin"], "manifest pet_art 不匹配");
  if (shared) require(!("requires_matching_pet_art" in standalone), "shared game art must replace pet requirement");
  const matching = object(shared ? standalone.requires_matching_shared_game_art : standalone.requires_matching_pet_art, "standalone matching shared/pet art");
  require(standalone.file === "trae_card.bin" && standalone.offset === 0x10000 && standalone.sha256 === hashes["trae_card.bin"] &&
    standalone.complete_flash_image === false && Object.keys(matching).length === 3 && matching.file === "petart.bin" &&
    matching.offset === 0x500000 && matching.sha256 === hashes["petart.bin"], "独立应用缺少正确的匹配图包要求");
  if (hasCompanion) {
    const data = payload["companion.bin"], v = view(data);
    const version = v.getUint32(4,true);
    require(data.length === 0x200000 && ascii(data,0,4) === "CPR1" && [1,2,3].includes(version), "companion 头/容量错误");
    const used = v.getUint32(8,true);
    require(used >= 128 && used <= data.length && crc32(data.subarray(16,used)) === v.getUint32(12,true), "companion 长度/CRC 错误");
    let end = 128;
    const count = version === 1 ? 4 : 14, frameBytes = version === 1 ? 96*156*3 : 144*144*3;
    for (let i=0;i<count;i++) {
      const offset=v.getUint32(16+i*8,true), size=v.getUint32(20+i*8,true);
      require(offset>=end && offset%4===0 && size>0 && offset<=used && size<=used-offset &&
        (i<2 || size===frameBytes), "companion 资源范围错误"); end=offset+size;
    }
    if(version===3){
      const at=v.getUint32(16,true),n=v.getUint32(20,true);
      const bodyDigest=Array.from(data.subarray(at,at+n),b=>b.toString(16).padStart(2,"0")).join("");
      require(n===32 && contains(app,terminated(ENCODER.encode(`BODYFONT-SHA256:${bodyDigest}`))),"companion body font 指纹不匹配");
    }
    const fingerprint=await sha256(data.subarray(0,used));
    require(contains(app,terminated(ENCODER.encode(`COMPANION-SHA256:${fingerprint}`))), "应用与 companion 指纹不匹配");
    const required = object(standalone.requires_matching_companion,"standalone matching companion");
    require(required.file === "companion.bin" && required.offset === 0x600000 && required.sha256 === hashes["companion.bin"], "独立应用缺少正确的 companion 要求");
  }
  const flashFiles = object(json("flasher_args.json").flash_files, "flash_files");
  require(Object.keys(flashFiles).length === files.length && files.every((item) => flashFiles[`0x${item.address.toString(16)}`] === item.name),
    "flasher_args.json 必须是准确的扁平路径");
  const tokens = DECODER.decode(payload.flash_args).split(/\s+/), textFiles = new Map<number, string>();
  tokens.forEach((token, index) => {
    if (/^0x[0-9a-f]+$/i.test(token)) {
      const offset = Number(token); require(!textFiles.has(offset), "flash_args offset 重复"); textFiles.set(offset, tokens[index + 1]);
    }
  });
  require(textFiles.size === files.length && files.every((item) => textFiles.get(item.address) === item.name), "flash_args 写入文件/offset 错误");
  const combined = payload["FONT-LICENSES.txt"], sources = json("font-sources.json"), embedded = object(sources.embedded_notice, "font embedded_notice");
  require(combined.length && contains(app, terminated(combined)) && embedded.file === "FONT-LICENSES.txt" &&
    embedded.bytes === combined.length && embedded.sha256 === hashes["FONT-LICENSES.txt"], "应用 BIN 缺少完整字体许可原文或许可目录不匹配");
  const licenses = array(sources.licenses, "font licenses").map((item) => object(item, "font license"));
  for (const name of FONT_LICENSES) {
    const raw = payload[name], records = licenses.filter((item) => item.file === name);
    require(records.length === 1 && records[0].bytes === raw.length && records[0].sha256 === hashes[name] &&
      contains(raw, ENCODER.encode("SIL OPEN FONT LICENSE")) && contains(combined, raw), `字体许可原文不完整: ${name}`);
  }
  const notices = object(manifest.font_notices, "font_notices");
  require(notices.file === "FONT-LICENSES.txt" && notices.sha256 === hashes["FONT-LICENSES.txt"] &&
    notices.embedded_in_application === true, "manifest 字体许可声明错误");
  return { name: file.name, files };
}
