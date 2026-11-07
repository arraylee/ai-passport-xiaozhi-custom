"""Losslessly pack the existing 24 kHz recording in 120-sample DMA blocks."""
from pathlib import Path
import struct
import zlib

def encode(raw):
    samples=[a for a, in struct.iter_unpack("<h",raw)]
    out=bytearray(struct.pack("<4sII",b"RCE1",len(samples),zlib.crc32(raw)))
    older=previous=0
    for at in range(0,len(samples),120):
        block=samples[at:at+120]; candidates=[]
        for order in range(3):
            prev,prev2=previous,older;values=[]
            for sample in block:
                delta=sample-(0,prev,2*prev-prev2)[order]
                values.append(delta*2 if delta>=0 else -delta*2-1)
                prev2,prev=prev,sample
            for k in range(16):
                bits=len(block)*(k+1)+sum(v>>k for v in values)
                candidates.append((bits,order,k,values))
        bits,order,k,values=min(candidates)
        if bits>=16*len(block):
            out.append(255);out.extend(struct.pack("<"+"h"*len(block),*block))
        else:
            out.append(order*16+k)
            binary="".join("0"*(v>>k)+"1"+(format(v&((1<<k)-1),f"0{k}b") if k else "") for v in values)
            binary+="0"*(-len(binary)%8);out.extend(int(binary,2).to_bytes(len(binary)//8,"big"))
        older,previous=block[-2:]
    return out

if __name__=="__main__":
    root=Path(__file__).resolve().parents[1]/"components/platform/xiaozhi"
    packed=encode((root/"welcome_zh.pcm").read_bytes())
    (root/"welcome_zh.rice").write_bytes(packed)
    print(f"{len(packed)} bytes")

