import { ESPLoader, Transport, type IEspLoaderTerminal } from "esptool-js";
import type { FirmwareFile } from "./release-loader";

export interface ConnectedDevice { chip: string; sn: string }

export class CardFlasher {
  private transport?: Transport;
  private loader?: ESPLoader;

  constructor(private readonly terminal: IEspLoaderTerminal) {}

  async connect(): Promise<ConnectedDevice> {
    if (!("serial" in navigator)) throw new Error("浏览器不支持 Web Serial，请使用最新版 Chrome 或 Edge");
    const port = await navigator.serial.requestPort();
    this.transport = new Transport(port, false);
    this.loader = new ESPLoader({ transport: this.transport, baudrate: 460800, terminal: this.terminal });
    const chip = await this.loader.main("default_reset");
    const description = await this.loader.chip.getChipDescription(this.loader);
    if (!description.toUpperCase().includes("ESP32-C3")) {
      throw new Error(`芯片型号不匹配：检测到 ${description}，要求 ESP32-C3`);
    }
    const mac = await this.loader.chip.readMac(this.loader);
    const sn = mac.replaceAll(":", "").toLowerCase();
    if (!/^[0-9a-f]{12}$/.test(sn)) throw new Error(`无法解析设备 MAC：${mac}`);
    return { chip: String(chip), sn };
  }

  async flash(files: FirmwareFile[], progress: (percent: number, label: string) => void): Promise<void> {
    if (!this.loader) throw new Error("请先连接设备");
    const total = files.reduce((sum, file) => sum + file.data.length, 0);
    const completed = files.map(() => 0);
    await this.loader.writeFlash({
      fileArray: files.map(({ address, data }) => ({ address, data })),
      flashMode: "dio",
      flashFreq: "80m",
      flashSize: "8MB",
      eraseAll: false,
      compress: true,
      reportProgress: (index, written, fileTotal) => {
        completed[index] = Math.min(written, fileTotal);
        const done = completed.reduce((sum, value) => sum + value, 0);
        progress(Math.round(done * 100 / total), files[index]?.name ?? "烧录");
      },
    });
    progress(100, "烧录完成，正在执行软件重启");
    await this.restartEsp32C3();
  }

  private async restartEsp32C3(): Promise<void> {
    if (!this.loader) throw new Error("设备未连接");
    // esptool-js 0.6.1 在 ESP32-C3 Stub 下不支持 softReset(false)，因此按官方
    // esptool.py ESP32-C3 watchdog_reset 的寄存器序列触发芯片级软件复位。
    const base = 0x60008000;
    const config0 = base + 0x90;
    const config1 = base + 0x94;
    const protect = base + 0xa8;
    try {
      await this.loader.writeReg(protect, 0x50d83aa1);
      await this.loader.writeReg(config1, 2000);
      await this.loader.writeReg(config0, (1 << 31) | (5 << 28) | (1 << 8) | 2);
      await this.loader.writeReg(protect, 0);
      await new Promise((resolve) => setTimeout(resolve, 700));
      this.terminal.writeLine("ESP32-C3 watchdog software reset triggered.");
    } catch (softwareError) {
      this.terminal.writeLine(`Software reset failed, falling back to RTS: ${String(softwareError)}`);
      await this.loader.after("hard_reset");
    } finally {
      await this.transport?.disconnect().catch(() => undefined);
      this.loader = undefined;
      this.transport = undefined;
    }
  }

  async disconnect(): Promise<void> {
    const transport = this.transport;
    this.loader = undefined;
    this.transport = undefined;
    await transport?.disconnect();
  }
}
