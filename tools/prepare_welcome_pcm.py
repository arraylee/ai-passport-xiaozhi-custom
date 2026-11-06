"""Decode the supplied welcome MP3 to native 24 kHz PCM; requires miniaudio 1.61."""
from pathlib import Path
import hashlib
import json
import sys


def main():
    import miniaudio
    root = Path(__file__).resolve().parents[1]
    source = root / "assets/source/audio/user-welcome.mp3"
    target = root / "components/platform/xiaozhi/welcome_zh.pcm"
    decoded = miniaudio.decode_file(str(source), output_format=miniaudio.SampleFormat.SIGNED16,
                                   nchannels=1, sample_rate=24000)
    samples = decoded.samples
    original_samples = len(samples)
    peak = max(abs(sample) for sample in samples)
    assert 24000 < original_samples < 24000 * 10
    assert peak < 32760, "Source contains clipped samples; do not hide distortion with normalization"
    # Remove only the very quiet edges (< -60 dBFS), with 80 ms before the
    # first audible sample and 100 ms after the last. Keep every interior
    # sample unchanged, including pauses. The final 100 ms drains the DMA.
    audible = [i for i, sample in enumerate(samples) if abs(sample) > 32]
    assert audible, "Empty welcome recording"
    start = max(0, audible[0] - 1920)
    end = min(original_samples, audible[-1] + 1 + 2400)
    samples = samples[start:end]
    trimmed_samples = len(samples)
    padded = (trimmed_samples + 119) // 120 * 120
    samples.extend([0] * (padded - trimmed_samples))
    if sys.byteorder != "little":
        samples.byteswap()
    pcm = samples.tobytes()
    target.write_bytes(pcm)
    digest = lambda data: hashlib.sha256(data).hexdigest()
    metadata = dict(source=source.relative_to(root).as_posix(), source_sha256=digest(source.read_bytes()),
                    format="PCM-S16LE", sample_rate=24000, channels=1, original_samples=original_samples,
                    samples=padded, bytes=len(pcm), seconds=padded/24000, source_peak=peak,
                    retained_source_start=start, retained_source_end=end, silence_threshold=32,
                    added_silence_samples=padded-trimmed_samples, sha256=digest(pcm),
                    processing="MP3 decode and trim quiet outer edges with 80/100 ms margins; no synthesis, time stretching, gain or further lossy encoding")
    (root / "assets/source/audio/welcome-pcm.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (root / "assets/licenses/WELCOME-SOURCE.txt").write_text(
        "开机配网引导录音\n用户于 2026-11-06 提供并指定用于本设备固件。\n"
        "原文件：c366510d-5354-40de-8728-f1273e3df3c2.mp3\n"
        f"原文件 SHA-256：{metadata['source_sha256']}\n"
        "保留原录音的声音、语气、采样率和音量；解码为 24 kHz 单声道 PCM16。\n"
        "仅裁掉首尾极低电平静音，保留前 80 毫秒、后 100 毫秒余量；中间样本逐一保持原值。\n"
        f"固件内音频 SHA-256：{metadata['sha256']}\n"
        "该用户提供的录音没有因此获得开源许可；固件源代码许可不自动适用于录音。\n")
    print(json.dumps(metadata, ensure_ascii=False))


if __name__ == "__main__":
    main()
