import "./style.css";
import type { IEspLoaderTerminal } from "esptool-js";
import { CardFlasher } from "./flasher";
import { generateCardIdPartition, validateCardId } from "./cardid-nvs";
import { downloadLedger } from "./ledger";
import { loadRelease, type LoadedRelease } from "./release-loader";

const el = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const releaseInput = el<HTMLInputElement>("release");
const keyInput = el<HTMLInputElement>("key");
const pkInput = el<HTMLInputElement>("pk");
const hwInput = el<HTMLInputElement>("hw");
const connectButton = el<HTMLButtonElement>("connect");
const flashButton = el<HTMLButtonElement>("flash");
const status = el<HTMLDivElement>("status");
const progress = el<HTMLProgressElement>("progress");
const log = el<HTMLPreElement>("log");

let release: LoadedRelease | undefined;
let sn = "";
let busy = false;
let loading = false;
let connection: "disconnected" | "connecting" | "connected" = "disconnected";
let releaseGeneration = 0;
let operationGeneration = 0;

function appendLog(text: string): void {
  log.textContent += text;
  log.scrollTop = log.scrollHeight;
}

const terminal: IEspLoaderTerminal = {
  clean: () => { log.textContent = ""; },
  write: (data) => appendLog(data),
  writeLine: (data) => appendLog(`${data}\n`),
};
const flasher = new CardFlasher(terminal);

function setStatus(message: string, error = false): void {
  status.textContent = message;
  status.classList.toggle("error", error);
}

function identityInput() {
  return {
    sn,
    deviceSecret: keyInput.value.trim(),
    productKey: pkInput.value.trim(),
    hardwareVersion: hwInput.value.trim(),
  };
}

function readyToFlash(): boolean {
  if (busy || loading || connection !== "connected" || !release) return false;
  try { validateCardId(identityInput()); return true; }
  catch { return false; }
}

function updateReady(): void {
  const locked = busy || connection === "connecting";
  for (const input of [releaseInput, keyInput, pkInput, hwInput]) input.disabled = locked;
  connectButton.disabled = locked || loading || connection === "connected";
  flashButton.disabled = !readyToFlash();
}

function clearDevice(): void {
  sn = "";
  el("device").textContent = "未连接";
  el("sn").textContent = "------------";
}

async function selectRelease(file?: File): Promise<void> {
  // Disabled controls also need a handler guard against queued/synthetic events.
  if (busy || connection === "connecting") return;
  const generation = ++releaseGeneration;
  release = undefined;
  loading = Boolean(file);
  progress.value = 0;
  setStatus(file ? `正在校验 ${file.name}……` : "已取消选择，请重新选择 release ZIP。");
  updateReady();
  if (!file) return;
  try {
    const loaded = await loadRelease(file);
    if (generation !== releaseGeneration) return;
    release = loaded;
    setStatus(`已载入 ${loaded.name}，包含 ${loaded.files.length} 个固定分区。`);
  } catch (error) {
    if (generation !== releaseGeneration) return;
    setStatus(error instanceof Error ? error.message : String(error), true);
  } finally {
    if (generation === releaseGeneration) {
      loading = false;
      updateReady();
    }
  }
}

releaseInput.addEventListener("change", () => selectRelease(releaseInput.files?.[0]));
releaseInput.addEventListener("cancel", () => {
  if (busy || connection === "connecting") return;
  releaseInput.value = "";
  return selectRelease();
});

for (const input of [keyInput, pkInput, hwInput]) input.addEventListener("input", updateReady);

connectButton.addEventListener("click", async () => {
  if (busy || loading || connection !== "disconnected") return;
  connection = "connecting";
  ++operationGeneration;
  clearDevice();
  updateReady();
  try {
    setStatus("正在进入 ROM 下载模式并识别芯片……");
    const device = await flasher.connect();
    sn = device.sn;
    el("device").textContent = device.chip;
    el("sn").textContent = sn;
    connection = "connected";
    connectButton.textContent = "已连接";
    setStatus("设备连接成功，可以开始烧录。");
  } catch (error) {
    await flasher.disconnect().catch(() => undefined);
    clearDevice();
    connection = "disconnected";
    connectButton.textContent = "连接 ESP32-C3";
    setStatus(error instanceof Error ? error.message : String(error), true);
  } finally {
    updateReady();
  }
});

flashButton.addEventListener("click", async () => {
  if (!readyToFlash() || !release) return;
  // Never read mutable UI/release state after awaiting the serial operation.
  const snapshot = {
    identity: identityInput(),
    releaseName: release.name,
    files: release.files.map((file) => ({ ...file, data: file.data.slice() })),
  };
  const generation = ++operationGeneration;
  busy = true;
  updateReady();
  progress.value = 0;
  try {
    const cardid = generateCardIdPartition(snapshot.identity);
    // cardid 最后写：固定固件分区失败时不在设备上留下已生效但未导出台账的身份。
    const files = [
      ...snapshot.files,
      { name: "cardid.bin", address: 0x356000, data: cardid },
    ];
    setStatus("正在烧录，请勿断电或拔线……");
    await flasher.flash(files, (percent, label) => {
      if (!busy || generation !== operationGeneration) return;
      progress.value = percent;
      setStatus(`${label}：${percent}%`);
    });
    downloadLedger({
      sn: snapshot.identity.sn,
      key: snapshot.identity.deviceSecret,
      hw: snapshot.identity.hardwareVersion,
      release: snapshot.releaseName,
      timestamp: new Date().toISOString(),
    });
    keyInput.value = "";
    setStatus("烧录成功，设备已通过看门狗软件重启；本机身份台账已下载，请妥善保存。");
  } catch (error) {
    setStatus(`烧录失败：${error instanceof Error ? error.message : String(error)}`, true);
  } finally {
    ++operationGeneration; // Ignore delayed progress while disconnecting/next device.
    await flasher.disconnect().catch(() => undefined);
    clearDevice();
    connection = "disconnected";
    connectButton.textContent = "连接下一台 ESP32-C3";
    busy = false;
    updateReady();
  }
});

updateReady();
