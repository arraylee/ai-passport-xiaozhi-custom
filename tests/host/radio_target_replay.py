"""Run the shipping ESP32-C3 radio player, including its MP3 decoder, on captured streams.

Requires unicorn 2.1 and pyelftools. The ELF supplies all executable code. Only
HTTP, audio HAL, RTOS scheduling, system heap and ROM operations are substituted.
The two target tasks share memory and switch at the real blocking boundaries.
"""
import argparse
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32, UC_HOOK_CODE, UC_HOOK_MEM_INVALID, UC_HOOK_MEM_WRITE
from unicorn.riscv_const import (UC_RISCV_REG_A0, UC_RISCV_REG_PC, UC_RISCV_REG_RA,
                                 UC_RISCV_REG_SP, UC_RISCV_REG_GP)

def replay(elf, sample, cancel_after=0, fail_allocation=0, fail_http=False, fail_task=False,
           public_http=False, fail_stream=False):
    cpu = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
    symbols = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}
    pages = set()
    def map_region(start, size):
        for page in range(start & ~4095, (start+size+4095) & ~4095, 4096):
            if page not in pages:
                cpu.mem_map(page, 4096)
                pages.add(page)
    for segment in elf.iter_segments():
        if segment["p_type"] == "PT_LOAD":
            start = segment["p_vaddr"]
            map_region(start, segment["p_memsz"])
            cpu.mem_write(start, segment.data())
    map_region(0x40000000, 0x10000)
    base = 0x20000000
    map_region(base, 0x100000)
    def word(at, *values):
        cpu.mem_write(at, struct.pack("<"+"I"*len(values), *values))
    def reg(i):
        return cpu.reg_read(UC_RISCV_REG_A0+i)
    def set_arg(i, value):
        cpu.reg_write(UC_RISCV_REG_A0+i, value)
    def finish(value=0):
        set_arg(0, value)
        cpu.reg_write(UC_RISCV_REG_PC, cpu.reg_read(UC_RISCV_REG_RA))
    def hook(at, func):
        cpu.hook_add(UC_HOOK_CODE, lambda *_: func(), begin=at, end=at)
    def system(name, func):
        if name in symbols:
            if 0x40000000 <= symbols[name] < 0x40010000:
                word(symbols[name], 0x00008067)
            hook(symbols[name], func)

    contexts, running, pending = {}, "network", None
    audio_suspended = False
    task_created = task_deleted = 0
    task_stack = 0
    create_calls = 0
    stack_low = {"network": base+0xff000, "audio": base+0x20000}
    switches = 0
    def switch():
        nonlocal pending, switches
        other = "audio" if running == "network" else "network"
        if other not in contexts or (other == "audio" and audio_suspended): return
        contexts[running] = cpu.context_save()
        pending = other
        cpu.emu_stop()  # Restore outside the instruction callback / translated block.
        switches += 1
        assert switches < 20000, "task deadlock"
    def create_task():
        nonlocal task_created, task_stack, audio_suspended, create_calls
        assert task_created == task_deleted
        create_calls += 1
        if fail_task: finish(0); return
        entry, task_stack, argument, handle = reg(0), reg(2), reg(3), reg(5)
        assert task_stack == 6144 and reg(4)==7
        word(handle, 0x2222)
        finish(1)
        parent = cpu.context_save()
        cpu.reg_write(UC_RISCV_REG_PC, entry)
        cpu.reg_write(UC_RISCV_REG_SP, base+0x20000)
        cpu.reg_write(UC_RISCV_REG_RA, base+0x12c)
        set_arg(0, argument)
        contexts["audio"] = cpu.context_save()
        cpu.context_restore(parent)
        task_created += 1
        audio_suspended = False
        switch()  # Higher-priority audio task waits for its first network data.
    def delay():
        finish(); switch()
    def suspend():
        nonlocal audio_suspended
        assert running == "audio" and not reg(0)
        audio_suspended = True
        switch()
    def delete_task():
        nonlocal task_deleted
        assert running == "network" and audio_suspended and reg(0)==0x2222
        task_deleted += 1; finish()
    def stack_write(uc, access, address, size, value, arg):
        if base+0x10000 <= address < base+0x20000:
            stack_low["audio"] = min(stack_low["audio"], address)
            assert address >= base+0x20000-task_stack, "audio stack overflow"
        elif base+0xf0000 <= address < base+0xff000:
            stack_low["network"] = min(stack_low["network"], address)
    cpu.hook_add(UC_HOOK_MEM_WRITE,stack_write,begin=base+0x10000,end=base+0x1ffff)
    cpu.hook_add(UC_HOOK_MEM_WRITE,stack_write,begin=base+0xf0000,end=base+0xfefff)
    system("xTaskCreate",create_task)
    system("xTaskCreatePinnedToCore",create_task)
    system("vTaskDelay",delay)
    system("vTaskSuspend",suspend)
    system("vTaskDelete",delete_task)
    system("uxTaskGetStackHighWaterMark",lambda:finish(1000))
    system("xz_codec_memory_radio_enter",lambda:finish(1))
    system("xz_codec_memory_radio_leave",finish)

    heap = base+0x30000
    live = {}
    peak = 0
    streaming = False
    allocations = 0
    def allocate(size):
        nonlocal heap, peak, allocations
        allocations += 1
        if allocations == fail_allocation:
            finish(0)
            return
        ptr = heap
        heap += (size+15) & ~15
        assert heap <= base+0xe0000
        live[ptr] = size
        peak = max(peak, sum(live.values()))
        finish(ptr)
    def release(ptr):
        if ptr:
            del live[ptr]
        finish()
    def copy():
        cpu.mem_write(reg(0),bytes(cpu.mem_read(reg(1),reg(2))))
        finish(reg(0))
    system("malloc",lambda:allocate(reg(0)))
    system("calloc",lambda:allocate(reg(0)*reg(1)))
    system("free",lambda:release(reg(0)))
    system("media_lib_module_malloc",lambda:allocate(reg(1)))
    system("media_lib_module_calloc",lambda:allocate(reg(1)*reg(2)))
    system("media_lib_free",lambda:release(reg(0)))
    system("memcpy",copy)
    system("memmove",copy)
    system("memset",lambda:(cpu.mem_write(reg(0),bytes([reg(1)&255])*reg(2)),finish(reg(0))))
    def cstring(at, limit=65536):
        value = bytearray()
        for i in range(limit):
            b = cpu.mem_read(at+i,1)[0]
            if not b: break
            value.append(b)
        return bytes(value)
    system("strlen",lambda:finish(len(cstring(reg(0)))))
    system("strncmp",lambda:finish(0 if cstring(reg(0),reg(2))==cstring(reg(1),reg(2)) else 1))
    system("esp_log_write",finish)
    system("esp_log_timestamp",lambda:finish(0))
    system("esp_log_early_timestamp",lambda:finish(0))
    system("esp_log",finish)
    system("esp_timer_get_time",lambda:(set_arg(1,0),finish(0)))
    def divide64(with_remainder=False):
        value = reg(0) | (reg(1)<<32)
        divisor = reg(2) | (reg(3)<<32)
        quotient, remainder = divmod(value,divisor)
        if with_remainder and reg(4): cpu.mem_write(reg(4),struct.pack("<Q",remainder))
        set_arg(1,quotient>>32);finish(quotient&0xffffffff)
    system("__udivdi3",divide64)
    system("__udivmoddi4",lambda:divide64(True))
    system("esp_rom_printf",finish)
    system("printf",finish)
    # The decoder selects its C3 implementation from the hardware revision.
    system("efuse_hal_get_major_chip_version",lambda:finish(0))
    system("efuse_hal_get_minor_chip_version",lambda:finish(4))

    code = base+0x100
    config, audio, ops, station, data = [base+n for n in (0x1000,0x1100,0x1200,0x1300,0x4000)]
    chunks = memoryview(sample)
    feed, ctx, resume, offset = 0,0,0,0
    frames, amplitude, rate, acquired, released = 0,0,0,0,0
    sizes = (1,7,511,1024,373)
    turn = 0
    def send_chunk():
        nonlocal turn, offset
        if offset == len(chunks) or (cancel_after and frames>=cancel_after):
            cpu.reg_write(UC_RISCV_REG_RA,resume)
            finish(int(offset == len(chunks) and not cancel_after and not fail_stream))
            return
        length = min(sizes[turn%len(sizes)], len(chunks)-offset)
        set_arg(0,data+offset); set_arg(1,length); set_arg(2,ctx)
        offset += length; turn += 1
        cpu.reg_write(UC_RISCV_REG_RA,code)
        cpu.reg_write(UC_RISCV_REG_PC,feed)
    def visit():
        nonlocal feed, ctx, resume, streaming, offset, turn
        assert not live, "decoder must not overlap TLS handshake"
        streaming = True
        if fail_http:
            finish(0)
            return
        feed, ctx, resume, offset, turn = reg(1), reg(2), cpu.reg_read(UC_RISCV_REG_RA), 0, 0
        send_chunk()
    def after_read():
        if reg(0):
            send_chunk()
        else:
            cpu.reg_write(UC_RISCV_REG_RA,resume); finish(int(not (cancel_after and frames>=cancel_after)))
    def acquire():
        nonlocal acquired
        assert streaming, "preparation must not open the speaker"
        acquired+=1; finish(1)
    def close_audio():
        nonlocal released
        released+=1; finish()
    def clock():
        nonlocal rate
        rate=reg(1); assert reg(2)==16 and reg(3)==1
        finish()
    def write():
        nonlocal frames, amplitude
        n=reg(2); assert n==240, "every speaker write must be one complete 120-frame DMA block"
        pcm=struct.unpack("<"+"h"*(n//2),cpu.mem_read(reg(1),n))
        amplitude=max(amplitude,*map(abs,pcm))
        frames+=1; word(reg(3),n); finish()
    system("xz_online_cancelled",lambda:finish(int(bool(cancel_after and frames>=cancel_after))))
    system("xz_http_visit",visit)
    hook(code,after_read)
    hook(code+4,acquire);hook(code+8,close_audio);hook(code+12,clock)
    hook(code+16,write);hook(code+20,finish)
    # Legal target instructions let Unicorn translate the boundary trampolines.
    for addr in range(code,code+28,4): word(addr,0x00008067)
    word(config,audio,code+4,code+8)
    word(audio,ops,0)
    word(ops,code+16,0,0,code+12,code+20,0,code+20,0,0)
    cpu.mem_write(data,sample)
    cpu.mem_write(station+80,b"http://example.org/stream.mp3\0" if public_http else b"https://example.org/stream.mp3\0")
    cpu.reg_write(UC_RISCV_REG_GP,symbols.get("__global_pointer$",0))
    cpu.reg_write(UC_RISCV_REG_SP,base+0xff000)
    cpu.reg_write(UC_RISCV_REG_RA,code+24)
    set_arg(0,config);set_arg(1,station)
    def bad_access(uc, access, address, size, value, arg):
        raise RuntimeError(f"unmapped {address:#x} PC={uc.reg_read(UC_RISCV_REG_PC):#x}")
    cpu.hook_add(UC_HOOK_MEM_INVALID,bad_access)
    try:
        cpu.reg_write(UC_RISCV_REG_PC,symbols["xz_radio_stream"])
        while pending or cpu.reg_read(UC_RISCV_REG_PC)!=code+24:
            if pending:
                running, pending = pending, None
                cpu.context_restore(contexts[running])
            cpu.emu_start(cpu.reg_read(UC_RISCV_REG_PC),code+24,count=150_000_000)
    except Exception as exc:
        pc=cpu.reg_read(UC_RISCV_REG_PC)
        near=max(((v,k) for k,v in symbols.items() if v<=pc),default=(0,""))
        raise RuntimeError(f"PC {pc:#x} near {near} frames={frames}, heap={live}") from exc
    failed = bool(fail_allocation or fail_http or fail_task or fail_stream)
    assert cpu.reg_read(UC_RISCV_REG_PC)==code+24 and (bool(reg(0)) != bool(cancel_after or failed))
    assert acquired==released==(3 if fail_stream else int(not failed)) and not live, (acquired,released,live)
    assert task_created==task_deleted==(0 if fail_task else 3 if (fail_stream or fail_http) else 1)
    if failed and not fail_stream:
        assert frames==0 and (not fail_allocation or allocations>=fail_allocation)
    elif cancel_after:
        # A live MP3 bit reservoir can legitimately yield silence in the first frames.
        assert frames==cancel_after, (frames,cancel_after)
    else:
        assert frames>=3 and amplitude>0, (frames,amplitude)
    return {"dma_blocks":frames,"rate":rate,"peak_pcm":amplitude,"decoder_heap_peak":peak,
            "audio_stack_used":base+0x20000-stack_low["audio"],
            "network_stack_used_without_tls":base+0xff000-stack_low["network"],"task_switches":switches}

if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("elf",type=Path)
    p.add_argument("samples",nargs="+",type=Path)
    args=p.parse_args()
    with args.elf.open("rb") as image:
        firmware=ELFFile(image)
        for path in args.samples:
            print(path.stem, replay(firmware,path.read_bytes()),flush=True)
        print("8 KiB HTTP queue wrap",replay(firmware,args.samples[0].read_bytes()*6,public_http=True),flush=True)
        print("midstream loss, bounded reconnect",replay(firmware,args.samples[0].read_bytes(),fail_stream=True),flush=True)
        print("stop after 3 DMA blocks",replay(firmware,args.samples[0].read_bytes(),cancel_after=3),flush=True)
        print("HTTP failure",replay(firmware,args.samples[0].read_bytes(),fail_http=True),flush=True)
        print("task allocation failure",replay(firmware,args.samples[0].read_bytes(),fail_task=True),flush=True)
        for failure in range(1,10):
            print("allocation failure",failure,replay(firmware,args.samples[0].read_bytes(),fail_allocation=failure),flush=True)

