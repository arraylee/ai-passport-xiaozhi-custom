const PAGE_SIZE = 4096;
const ENTRY_SIZE = 32;
const FIRST_ENTRY = 64;
const VERSION2 = 0xfe;
const ACTIVE = 0xfffffffe;
const TYPE_U8 = 0x01;
const TYPE_STRING = 0x21;

export interface CardIdInput {
  sn: string;
  deviceSecret: string;
  productKey: string;
  hardwareVersion: string;
}

function crcTable(): Uint32Array {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
}

const CRC_TABLE = crcTable();

// Matches Python zlib.crc32(data, 0xffffffff), used by ESP-IDF's generator.
export function espCrc32(data: Uint8Array): number {
  let crc = 0;
  for (const byte of data) crc = CRC_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

function putU16(target: Uint8Array, offset: number, value: number): void {
  new DataView(target.buffer, target.byteOffset, target.byteLength).setUint16(offset, value, true);
}

function putU32(target: Uint8Array, offset: number, value: number): void {
  new DataView(target.buffer, target.byteOffset, target.byteLength).setUint32(offset, value >>> 0, true);
}

function entryCrc(entry: Uint8Array): number {
  const crcData = new Uint8Array(28);
  crcData.set(entry.subarray(0, 4), 0);
  crcData.set(entry.subarray(8, 32), 4);
  return espCrc32(crcData);
}

class NvsPage {
  readonly data = new Uint8Array(PAGE_SIZE).fill(0xff);
  private readonly bitmap = new Uint8Array(32).fill(0xff);
  private entryCount = 0;

  constructor(sequence: number) {
    putU32(this.data, 0, ACTIVE);
    putU32(this.data, 4, sequence);
    this.data[8] = VERSION2;
    putU32(this.data, 28, espCrc32(this.data.slice(4, 28)));
  }

  private markWritten(): void {
    const bit = this.entryCount * 2;
    this.bitmap[bit >>> 3] &= ~(1 << (bit & 7));
    this.data.set(this.bitmap, 32);
    this.entryCount++;
  }

  private append(bytes: Uint8Array, spans: number): void {
    if (this.entryCount + spans > 126) throw new Error("cardid NVS 页面空间不足");
    this.data.set(bytes, FIRST_ENTRY + this.entryCount * ENTRY_SIZE);
    for (let i = 0; i < spans; i++) this.markWritten();
  }

  private baseEntry(namespace: number, type: number, span: number, key: string): Uint8Array {
    const rawKey = new TextEncoder().encode(key);
    if (rawKey.length > 15) throw new Error(`NVS key 过长: ${key}`);
    const entry = new Uint8Array(ENTRY_SIZE).fill(0xff);
    entry[0] = namespace;
    entry[1] = type;
    entry[2] = span;
    entry[3] = 0xff;
    entry.fill(0, 8, 24);
    entry.set(rawKey, 8);
    return entry;
  }

  writeNamespace(name: string, index: number): void {
    const entry = this.baseEntry(0, TYPE_U8, 1, name);
    entry[24] = index;
    putU32(entry, 4, entryCrc(entry));
    this.append(entry, 1);
  }

  writeString(namespace: number, key: string, value: string): void {
    const encoded = new TextEncoder().encode(`${value}\0`);
    const dataEntries = Math.ceil(encoded.length / ENTRY_SIZE);
    const entry = this.baseEntry(namespace, TYPE_STRING, dataEntries + 1, key);
    putU16(entry, 24, encoded.length);
    putU32(entry, 28, espCrc32(encoded));
    putU32(entry, 4, entryCrc(entry));
    this.append(entry, 1);
    const padded = new Uint8Array(dataEntries * ENTRY_SIZE).fill(0xff);
    padded.set(encoded);
    this.append(padded, dataEntries);
  }
}

export function validateCardId(input: CardIdInput): void {
  if (!/^[0-9a-f]{12}$/.test(input.sn)) throw new Error("SN 必须是读取到的 12 位小写十六进制 MAC");
  for (const [name, value, max] of [
    ["DeviceSecret", input.deviceSecret, 64],
    ["ProductKey", input.productKey, 16],
    ["HardwareVersion", input.hardwareVersion, 8],
  ] as const) {
    if (!value || new TextEncoder().encode(value).length > max) {
      throw new Error(`${name} 必须为 1–${max} 字节`);
    }
    if (value.includes("\0")) throw new Error(`${name} 不能包含 NUL`);
  }
}

export function generateCardIdPartition(input: CardIdInput): Uint8Array {
  validateCardId(input);
  const partition = new Uint8Array(0x4000).fill(0xff);
  const page = new NvsPage(0);
  page.writeNamespace("folotoy-key", 1);
  page.writeString(1, "DeviceKey", input.sn);
  page.writeString(1, "DeviceSecret", input.deviceSecret);
  page.writeString(1, "ProductKey", input.productKey);
  page.writeString(1, "HardwareVersion", input.hardwareVersion);
  partition.set(page.data, 0);
  // Remaining two empty pages and the final reserved page intentionally stay erased (0xff).
  return partition;
}
