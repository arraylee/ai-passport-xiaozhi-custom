import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { IEspLoaderTerminal } from "esptool-js";
import { CardFlasher } from "./flasher";

const mocks = vi.hoisted(() => ({
  disconnect: vi.fn(), writeFlash: vi.fn(), writeReg: vi.fn(), after: vi.fn(),
}));
vi.mock("esptool-js", () => ({
  Transport: class { disconnect = mocks.disconnect; },
  ESPLoader: class {
    main = async () => "ESP32-C3";
    chip = { getChipDescription: async () => "ESP32-C3", readMac: async () => "00:11:22:33:44:55" };
    writeFlash = mocks.writeFlash;
    writeReg = mocks.writeReg;
    after = mocks.after;
  },
}));
const terminal: IEspLoaderTerminal = { clean() {}, write() {}, writeLine() {} };

beforeEach(() => {
  vi.resetAllMocks(); vi.useFakeTimers();
  vi.stubGlobal("navigator", { serial: { requestPort: async () => ({}) } });
  mocks.disconnect.mockResolvedValue(undefined);
  mocks.writeFlash.mockResolvedValue(undefined);
  mocks.writeReg.mockResolvedValue(undefined);
});
afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });

describe("serial flasher boundaries", () => {
  it("writes only supplied explicit segments, never erases the whole chip, and resets", async () => {
    const flasher = new CardFlasher(terminal); await flasher.connect();
    const files = [0, 0x8000, 0x10000, 0x3fa000, 0x500000, 0x356000].map((address, index) =>
      ({ name: `${index}.bin`, address, data: new Uint8Array([index]) }));
    const pending = flasher.flash(files, vi.fn());
    await vi.advanceTimersByTimeAsync(700); await pending;
    expect(mocks.writeFlash).toHaveBeenCalledTimes(1);
    expect(mocks.writeFlash.mock.calls[0][0]).toMatchObject({
      eraseAll: false,
      fileArray: files.map(({ address, data }) => ({ address, data })),
    });
    expect(mocks.writeReg).toHaveBeenCalledTimes(4);
    expect(mocks.disconnect).toHaveBeenCalledTimes(1);
    await expect(flasher.flash(files, vi.fn())).rejects.toThrow(/连接/);
  });

  it("forgets a stale serial session even when disconnect rejects", async () => {
    const flasher = new CardFlasher(terminal); await flasher.connect();
    mocks.disconnect.mockRejectedValueOnce(new Error("port disconnected"));
    await expect(flasher.disconnect()).rejects.toThrow("port disconnected");
    await expect(flasher.flash([], vi.fn())).rejects.toThrow(/连接/);
    expect(mocks.writeFlash).not.toHaveBeenCalled();
    await flasher.connect();
    await flasher.disconnect();
    expect(mocks.disconnect).toHaveBeenCalledTimes(2);
  });
});
