import { describe, expect, it } from "vitest";
import { generateCardIdPartition, validateCardId } from "./cardid-nvs";

function hex(bytes: ArrayBuffer): string {
  return Array.from(new Uint8Array(bytes), (byte) => byte.toString(16).padStart(2, "0")).join("");
}

describe("cardid NVS generator", () => {
  it("is byte-compatible with ESP-IDF nvs_partition_gen.py 0.1.9", async () => {
    const data = generateCardIdPartition({
      sn: "b81f3f450ada",
      deviceSecret: "0123456789abcdef0123456789abcdef",
      productKey: "folotoy",
      hardwareVersion: "A1.0",
    });
    expect(data).toHaveLength(0x4000);
    const digest = await crypto.subtle.digest("SHA-256", data.slice().buffer);
    expect(hex(digest)).toBe("9c21834e25b07ae86ed36ff3b81d3ee640cfe77bff51b4640d02ddb736b9d1e8");
  });

  it("rejects invalid identity fields", () => {
    expect(() => validateCardId({
      sn: "not-a-mac", deviceSecret: "x", productKey: "p", hardwareVersion: "A1",
    })).toThrow(/SN/);
    expect(() => validateCardId({
      sn: "b81f3f450ada", deviceSecret: "", productKey: "folotoy", hardwareVersion: "v1.0.0",
    })).toThrow(/DeviceSecret/);
    expect(() => validateCardId({
      sn: "b81f3f450ada", deviceSecret: "0123456789abcdef0123456789abcdef",
      productKey: "p", hardwareVersion: "x".repeat(9),
    })).toThrow(/HardwareVersion/);
  });

  it.each(["x", "x".repeat(64), "é".repeat(32), "密".repeat(21) + "x", "🔑".repeat(16)])(
    "accepts DeviceSecret within 1–64 UTF-8 bytes (%s)", (deviceSecret) => {
      const input = { sn: "b81f3f450ada", deviceSecret, productKey: "folotoy", hardwareVersion: "v1.0.0" };
      expect(() => validateCardId(input)).not.toThrow();
      const encoded = new TextEncoder().encode(`${deviceSecret}\0`);
      const image = generateCardIdPartition(input);
      // The DeviceSecret string is the fourth NVS entry after namespace + SN.
      const header = 64 + 3 * 32;
      expect(new DataView(image.buffer).getUint16(header + 24, true)).toBe(encoded.length);
      expect(image.slice(header + 32, header + 32 + encoded.length)).toEqual(encoded);
    },
  );

  it.each(["", "x".repeat(65), "é".repeat(32) + "x", "密".repeat(22), "🔑".repeat(16) + "x", "\0", "x\0y"])(
    "rejects out-of-range or NUL-containing DeviceSecret (%s)", (deviceSecret) => {
      expect(() => generateCardIdPartition({ sn: "b81f3f450ada", deviceSecret,
        productKey: "folotoy", hardwareVersion: "v1.0.0" })).toThrow(/DeviceSecret/);
    },
  );
});
