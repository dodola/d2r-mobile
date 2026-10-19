"""Conservative sampler facts from typed Maxwell instructions.

The decoding masks and fields follow the bundled Ryujinx InstTable and
InstDecoders. A bare integer 1D fetch remains ambiguous with a texture buffer;
queries alone never supply a type. Scheduling words and constant data are not
decoded as instructions. Conflicting uses are excluded rather than guessed.
"""
import struct

# Prefix masks from Ryujinx.Graphics.Shader/Decoders/InstTable.cs.
PREFIXES = {
    "Tex": "11000", "Texs": "1101100", "TexsF16": "1101000",
    "Tld": "11011100", "Tlds": "1101101", "TldsF16": "1101001",
    "Tld4": "110010", "Txd": "110111100", "TxdB": "1101111001",
}
DIMENSIONS = {0:"1d", 1:"1darray", 2:"2d", 3:"2darray", 4:"3d", 6:"cube", 7:"cubearray"}
TEXS = {0:"1d", **{x:"2d" for x in range(1,7)}, **{x:"2darray" for x in range(7,10)},
        10:"3d", 11:"3d", 12:"cube", 13:"cube"}
TLDS = {2:"2d", 4:"2d", 5:"2d", 6:"2dms", 7:"3d", 8:"2darray", 12:"2d"}


def infer(code, stage, program_size):
    if program_size > len(code) or program_size < (0 if stage=="cs" else 0x50):
        raise ValueError("Maxwell instruction range outside program")
    start=0 if stage=="cs" else 0x50
    patterns=sorted(PREFIXES.items(),key=lambda row:-len(row[1]))
    candidates, evidence = {}, []
    for group in range(start,program_size,32):
        for offset in (group+8,group+16,group+24):
            if offset+8>program_size:continue
            word,=struct.unpack_from("<Q",code,offset)
            name=next((name for name,prefix in patterns if word>>(64-len(prefix))==int(prefix,2)),None)
            if name is None or name=="TxdB":continue
            tid=(word>>36)&0x1fff
            if tid<8 or (tid-8)%2 or tid>=72:continue
            slot=(tid-8)//2
            if name.startswith("Texs"):kind=TEXS.get((word>>53)&15)
            elif name.startswith("Tlds"):kind=TLDS.get((word>>53)&15)
            else:
                dim=(word>>28)&7;kind=DIMENSIONS.get(dim)
                if name=="Tld":
                    lod=bool(word&(1<<55));ms=bool(word&(1<<50))
                    if dim==0 and not lod and not ms:kind=None
                    if ms:kind={2:"2dms",3:"2dmsarray"}.get(dim)
            if kind:
                candidates.setdefault(slot,set()).add(kind)
                evidence.append({"offset":offset,"opcode":name,"tid":tid,"slot":slot,"type":kind})
    conflicts={str(slot):sorted(kinds) for slot,kinds in candidates.items() if len(kinds)!=1}
    return {str(slot):next(iter(kinds)) for slot,kinds in candidates.items() if len(kinds)==1}, evidence, conflicts


def infer_images(code,stage,program_size):
    if program_size>len(code):raise ValueError("Image instruction range outside program")
    kinds={0:"1d",1:"buffer",2:"1darray",3:"2d",4:"2darray",5:"3d"}
    prefixes={"SuldD":"1110101100011","Suld":"1110101100001",
              "SustD":"1110101100111","Sust":"1110101100101"}
    found,evidence={},[]
    for group in range(0 if stage=="cs" else 0x50,program_size,32):
        for offset in (group+8,group+16,group+24):
            if offset+8>program_size:continue
            word,=struct.unpack_from("<Q",code,offset)
            name=next((name for name,prefix in prefixes.items() if word>>51==int(prefix,2)),None)
            if name is None:continue
            tid=(word>>36)&0x1fff;kind=kinds.get((word>>33)&7)
            if tid<72 or tid>=90 or (tid-72)%2 or kind is None:continue
            slot=(tid-72)//2;found.setdefault(slot,set()).add(kind)
            evidence.append({"offset":offset,"opcode":name,"tid":tid,"slot":slot,"type":kind})
    return {str(slot):{"type":next(iter(types))} for slot,types in found.items() if len(types)==1},evidence
