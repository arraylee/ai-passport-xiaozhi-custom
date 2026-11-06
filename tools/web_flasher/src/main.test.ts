import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { generateCardIdPartition } from "./cardid-nvs";
import type { LoadedRelease } from "./release-loader";

const mocks = vi.hoisted(() => ({
  connect: vi.fn(), disconnect: vi.fn(), flash: vi.fn(), loadRelease: vi.fn(), downloadLedger: vi.fn(),
}));
vi.mock("./flasher", () => ({ CardFlasher: class {
  connect = mocks.connect;
  disconnect = mocks.disconnect;
  flash = mocks.flash;
} }));
vi.mock("./release-loader", () => ({ loadRelease: mocks.loadRelease }));
vi.mock("./ledger", () => ({ downloadLedger: mocks.downloadLedger }));

// Exercise the actual main.ts event handlers without a browser or serial port.
// emit intentionally bypasses disabled controls to test the handler guards too.
class Element {
  value = "";
  textContent = "";
  disabled = false;
  files: File[] = [];
  scrollTop = 0;
  scrollHeight = 0;
  classList = { toggle: vi.fn() };
  private listeners = new Map<string, () => unknown>();
  addEventListener(event: string, listener: () => unknown): void { this.listeners.set(event, listener); }
  async emit(event: string): Promise<void> { await this.listeners.get(event)?.(); }
}

let nodes: Map<string, Element>;
function node(id: string): Element {
  if (!nodes.has(id)) nodes.set(id, new Element());
  return nodes.get(id)!;
}
function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: Error) => void;
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
function bundle(name: string): LoadedRelease {
  const names = ["bootloader.bin", "partition-table.bin", "trae_card.bin", "imgava.bin", "petart.bin"];
  return { name, files: [0, 0x8000, 0x10000, 0x3fa000, 0x500000].map((address, i) =>
    ({ name: names[i], address, data: new Uint8Array([name.charCodeAt(0), i]) })) };
}
function select(name?: string): Promise<void> {
  node("release").files = name ? [{ name } as File] : [];
  return node("release").emit("change");
}
async function ready(): Promise<void> {
  await select("A.zip"); await node("connect").emit("click");
  expect(node("flash").disabled).toBe(false);
}
function expectInputsLocked(locked: boolean): void {
  for (const id of ["release", "key", "pk", "hw"]) expect(node(id).disabled, id).toBe(locked);
}

beforeEach(async () => {
  vi.resetModules(); vi.resetAllMocks();
  nodes = new Map();
  vi.stubGlobal("document", { getElementById: node });
  node("key").value = "secret-a"; node("pk").value = "folotoy"; node("hw").value = "v1.0.0";
  mocks.connect.mockResolvedValue({ chip: "ESP32-C3", sn: "001122334455" });
  mocks.disconnect.mockResolvedValue(undefined);
  mocks.flash.mockResolvedValue(undefined);
  mocks.loadRelease.mockImplementation((file: File) => Promise.resolve(bundle(file.name)));
  await import("./main");
});
afterEach(() => { vi.unstubAllGlobals(); });

describe("flasher UI asynchronous ownership", () => {
  it("clears A immediately when B starts loading and cannot flash A during validation", async () => {
    await ready();
    const loading = deferred<LoadedRelease>(); mocks.loadRelease.mockReturnValueOnce(loading.promise);
    const pending = select("B.zip");
    expect(node("flash").disabled).toBe(true);
    await node("flash").emit("click"); expect(mocks.flash).not.toHaveBeenCalled();
    loading.resolve(bundle("B.zip")); await pending;
    expect(node("flash").disabled).toBe(false);
    await node("flash").emit("click");
    expect(mocks.flash.mock.calls[0][0][0].data[0]).toBe("B".charCodeAt(0));
    expect(mocks.downloadLedger.mock.calls[0][0].release).toBe("B.zip");
  });

  it.each([false, true])("clears selection on cancellation and ignores pending completion (cancel event=%s)", async (cancelEvent) => {
    await ready();
    const loading = deferred<LoadedRelease>(); mocks.loadRelease.mockReturnValueOnce(loading.promise);
    const pending = select("B.zip");
    if (cancelEvent) await node("release").emit("cancel"); else await select();
    const cancelledStatus = node("status").textContent;
    loading.resolve(bundle("B.zip")); await pending;
    expect(node("status").textContent).toBe(cancelledStatus);
    expect(node("flash").disabled).toBe(true);
    await node("flash").emit("click"); expect(mocks.flash).not.toHaveBeenCalled();
    await select("C.zip"); expect(node("flash").disabled).toBe(false);
  });

  it("never reuses A after B validation fails and allows a clean retry", async () => {
    await ready();
    mocks.loadRelease.mockRejectedValueOnce(new Error("bad ZIP"));
    await select("B.zip");
    expect(node("status").textContent).toBe("bad ZIP");
    expect(node("flash").disabled).toBe(true);
    await node("flash").emit("click"); expect(mocks.flash).not.toHaveBeenCalled();
    await select("C.zip"); expect(node("flash").disabled).toBe(false);
  });

  it.each([false, true])("ignores an older load completing after the latest (reject=%s)", async (rejectOld) => {
    await ready();
    const older = deferred<LoadedRelease>(), newer = deferred<LoadedRelease>();
    mocks.loadRelease.mockReturnValueOnce(older.promise).mockReturnValueOnce(newer.promise);
    const oldLoad = select("B.zip"), newLoad = select("C.zip");
    newer.resolve(bundle("C.zip")); await newLoad;
    const currentStatus = node("status").textContent;
    if (rejectOld) older.reject(new Error("stale failure")); else older.resolve(bundle("B.zip"));
    await oldLoad;
    expect(node("status").textContent).toBe(currentStatus);
    expect(node("flash").disabled).toBe(false);
    await node("flash").emit("click");
    expect(mocks.downloadLedger.mock.calls[0][0].release).toBe("C.zip");
  });

  it("an old failure cannot end the latest loading state or enable connect", async () => {
    const older = deferred<LoadedRelease>(), newer = deferred<LoadedRelease>();
    mocks.loadRelease.mockReturnValueOnce(older.promise).mockReturnValueOnce(newer.promise);
    const oldLoad = select("A.zip"), newLoad = select("B.zip");
    older.reject(new Error("stale failure")); await oldLoad;
    expect(node("connect").disabled).toBe(true);
    expect(node("status").textContent).toContain("B.zip");
    await node("connect").emit("click"); expect(mocks.connect).not.toHaveBeenCalled();
    newer.resolve(bundle("B.zip")); await newLoad;
    expect(node("connect").disabled).toBe(false);
  });

  it("snapshots image bytes and identity, locks inputs and ignores duplicate events until cleanup ends", async () => {
    const original = bundle("A.zip"); mocks.loadRelease.mockResolvedValueOnce(original);
    await ready();
    const writing = deferred<void>(), closing = deferred<void>();
    mocks.flash.mockReturnValueOnce(writing.promise); mocks.disconnect.mockReturnValueOnce(closing.promise);
    const operation = node("flash").emit("click");
    expectInputsLocked(true);
    expect(node("connect").disabled).toBe(true);
    const [files, report] = mocks.flash.mock.calls[0];
    expect(files.map((file: { address: number }) => file.address)).toEqual([0, 0x8000, 0x10000, 0x3fa000, 0x500000, 0x356000]);
    expect(files[5].data).toEqual(generateCardIdPartition({ sn: "001122334455", deviceSecret: "secret-a",
      productKey: "folotoy", hardwareVersion: "v1.0.0" }));
    node("key").value = "secret-b"; node("hw").value = "B2"; node("pk").value = "other";
    await node("key").emit("input"); await select("B.zip"); await node("release").emit("cancel");
    await node("flash").emit("click"); await node("connect").emit("click");
    original.name = "mutated.zip"; original.files[0].data[0] = 0;
    expect(files[0].data[0]).toBe("A".charCodeAt(0));
    expect(mocks.flash).toHaveBeenCalledTimes(1); expect(mocks.connect).toHaveBeenCalledTimes(1);
    expect(mocks.loadRelease).toHaveBeenCalledTimes(1);
    report(25, "writing A"); expect(node("status").textContent).toBe("writing A：25%");
    writing.resolve(); await vi.waitFor(() => expect(mocks.disconnect).toHaveBeenCalledTimes(1));
    expect(mocks.downloadLedger.mock.calls[0][0]).toMatchObject({ sn: "001122334455", key: "secret-a", hw: "v1.0.0", release: "A.zip" });
    const success = node("status").textContent;
    report(50, "late progress"); expect(node("status").textContent).toBe(success);
    expectInputsLocked(true); await node("flash").emit("click");
    expect(mocks.flash).toHaveBeenCalledTimes(1);
    closing.resolve(); await operation;
    expectInputsLocked(false); expect(node("connect").disabled).toBe(false);
    expect(node("flash").disabled).toBe(true); expect(node("key").value).toBe("");
    expect(node("sn").textContent).toBe("------------");
    report(90, "very late progress"); expect(node("status").textContent).toBe(success);
    node("key").value = "next-device-key";
    await node("connect").emit("click");
    const nextWrite = deferred<void>(); mocks.flash.mockReturnValueOnce(nextWrite.promise);
    const nextOperation = node("flash").emit("click");
    const nextStatus = node("status").textContent;
    report(100, "old operation"); expect(node("status").textContent).toBe(nextStatus);
    nextWrite.resolve(); await nextOperation;
  });

  it("serial connection is single-flight and ignores input events while connecting", async () => {
    await select("A.zip");
    const connecting = deferred<{ chip: string; sn: string }>(); mocks.connect.mockReturnValueOnce(connecting.promise);
    const pending = node("connect").emit("click");
    expectInputsLocked(true); expect(node("flash").disabled).toBe(true);
    await node("connect").emit("click"); await select("B.zip"); await node("flash").emit("click");
    expect(mocks.connect).toHaveBeenCalledTimes(1); expect(mocks.loadRelease).toHaveBeenCalledTimes(1);
    connecting.resolve({ chip: "ESP32-C3", sn: "001122334455" }); await pending;
    expectInputsLocked(false); expect(node("flash").disabled).toBe(false);
    await node("connect").emit("click"); expect(mocks.connect).toHaveBeenCalledTimes(1);
  });

  it("recovers after connect and disconnect errors without retaining a stale device", async () => {
    await select("A.zip");
    mocks.connect.mockRejectedValueOnce(new Error("connect failed"));
    mocks.disconnect.mockRejectedValueOnce(new Error("cleanup failed"));
    await node("connect").emit("click");
    expectInputsLocked(false); expect(node("connect").disabled).toBe(false);
    expect(node("flash").disabled).toBe(true); expect(node("sn").textContent).toBe("------------");
    expect(node("status").textContent).toBe("connect failed");
    await node("connect").emit("click"); expect(node("flash").disabled).toBe(false);
  });

  it.each(["flash", "ledger"])("recovers after %s exceptions and clears the device", async (failure) => {
    await ready();
    if (failure === "flash") mocks.flash.mockRejectedValueOnce(new Error("write failed"));
    else mocks.downloadLedger.mockImplementationOnce(() => { throw new Error("download failed"); });
    mocks.disconnect.mockRejectedValueOnce(new Error("cleanup failed"));
    await node("flash").emit("click");
    expectInputsLocked(false); expect(node("connect").disabled).toBe(false);
    expect(node("flash").disabled).toBe(true); expect(node("sn").textContent).toBe("------------");
    expect(node("status").textContent).toContain("失败");
    if (failure === "flash") expect(mocks.downloadLedger).not.toHaveBeenCalled();
    await node("connect").emit("click"); await node("flash").emit("click");
    expect(mocks.flash).toHaveBeenCalledTimes(2);
  });

  it("disables flash for invalid UTF-8 key lengths and NUL, including synthetic clicks", async () => {
    await ready();
    for (const key of ["", "x".repeat(65), "密".repeat(22), "🔑".repeat(16) + "x", "x\0y"]) {
      node("key").value = key; await node("key").emit("input");
      expect(node("flash").disabled).toBe(true);
      await node("flash").emit("click");
    }
    expect(mocks.flash).not.toHaveBeenCalled();
    node("key").value = "🔑".repeat(16); await node("key").emit("input");
    expect(node("flash").disabled).toBe(false);
  });
});
