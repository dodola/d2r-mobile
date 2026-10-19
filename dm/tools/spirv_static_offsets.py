"""Fold sample offsets only when proved constant from literals and NVN c1.

The source compiler can leave literal offsets in local variables or immutable
c1 words. SPIR-V ConstOffset requires actual constant declarations. Dynamic
guest buffers, aliasing stores, unsupported layouts and operations fail closed.
"""
import struct


def fold_sample_offsets(path,c1,cbufs):
    raw=path.read_bytes();words=list(struct.unpack(f"<{len(raw)//4}I",raw));instructions=[];i=5
    while i<len(words):
        count=words[i]>>16
        if not count or i+count>len(words):raise ValueError("Invalid offset instruction range")
        instructions.append(words[i:i+count]);i+=count
    definitions={x[1]:x for x in instructions if 19<=x[0]&65535<=33}
    definitions.update({x[2]:x for x in instructions if x[0]&65535 in (43,44,59,61,65,83,124,201,202,203)})
    constants={x[2] for x in instructions if x[0]&65535 in (41,42,43,44,46,48,49,50,51,52)}
    bad={x[2]:x for x in instructions if x[0]&65535==44 and any(v not in constants for v in x[3:])}
    if not bad:return 0
    bindings={x[1]:x[3] for x in instructions if x[0]&65535==71 and x[2]==33}
    c1_bindings={x["binding"] for x in cbufs if x["slot"]==1}
    c1_variables={vid for vid,inst in definitions.items() if inst[0]&65535==59 and bindings.get(vid) in c1_bindings}
    strides={x[1]:x[3] for x in instructions if x[0]&65535==71 and x[2]==6}
    member_offsets={(x[1],x[2]):x[4] for x in instructions if x[0]&65535==72 and x[3]==35}
    stores={}
    for inst in instructions:
        if inst[0]&65535==62:stores.setdefault(inst[1],[]).append(inst[2])
    active=set();memo={}

    def value(vid):
        if vid in memo:return memo[vid]
        if vid in active:raise ValueError("Offset expression contains a cycle")
        active.add(vid)
        inst=definitions.get(vid)
        if inst is None:raise ValueError("Offset expression has an unsupported value")
        op=inst[0]&65535
        if op==43:
            if len(inst)!=4:raise ValueError("Offset literal is not 32-bit")
            result=inst[3]
        elif op in (83,124):result=value(inst[3])
        elif op==61:
            pointer=inst[3];target=definitions.get(pointer)
            if target is None:raise ValueError("Offset load pointer is unknown")
            if target[0]&65535==59 and target[3]==7 and len(stores.get(pointer,[]))==1:
                if any(x[0]&65535==57 and pointer in x[4:] for x in instructions):
                    raise ValueError("Offset variable escapes to a function")
                result=value(stores[pointer][0])
            elif target[0]&65535==65 and target[3] in c1_variables and len(target)==7:
                var=definitions[target[3]];ptr=definitions[var[1]];structure=definitions[ptr[3]]
                if ptr[0]&65535!=32 or structure[0]&65535!=30 or len(structure)!=3:
                    raise ValueError("Unsupported c1 pointer/structure layout")
                array=definitions[structure[2]];vector=definitions[array[2]];scalar=definitions[vector[2]]
                if array[0]&65535!=28 or vector[0]&65535!=23 or vector[3]!=4 or scalar[0]&65535 not in (21,22) or scalar[2]!=32:
                    raise ValueError("Unsupported c1 array element type")
                if strides.get(array[1])!=16 or member_offsets.get((structure[1],0))!=0:
                    raise ValueError("Unsupported c1 array stride/member offset")
                member,index,component=map(value,target[4:])
                offset=index*16+component*4
                if member or component>=4 or offset+4>len(c1):raise ValueError("Offset read outside immutable c1")
                result=struct.unpack_from("<I",c1,offset)[0]
            else:raise ValueError("Offset depends on a dynamic or aliased load")
        elif op in (201,202,203):
            args=[value(x) for x in inst[3:]]
            source=args[0];offset,count=args[-2:]
            if offset+count>32:raise ValueError("Undefined offset bitfield range")
            mask=(1<<count)-1
            if op==201:result=(source&~(mask<<offset))|((args[1]&mask)<<offset)
            else:
                result=(source>>offset)&mask
                if op==202 and count and result&(1<<(count-1)):result|=~mask
        else:raise ValueError("Offset operation is not proven constant")
        active.remove(vid);memo[vid]=result&0xffffffff;return memo[vid]

    literal_ids={}
    next_id=words[3];out=[];repaired=0
    for inst in instructions:
        if inst[0]&65535==43 and len(inst)==4:literal_ids.setdefault((inst[1],inst[3]),inst[2])
        if inst[0]&65535==44 and inst[2] in bad:
            vector=definitions[inst[1]]
            if vector[0]&65535!=23:raise ValueError("Nonconstant offset is not a vector")
            component_type=vector[2];operands=[]
            for operand in inst[3:]:
                bits=value(operand);key=(component_type,bits)
                if key not in literal_ids:
                    literal_ids[key]=next_id;out.append([(4<<16)|43,component_type,next_id,bits]);next_id+=1
                operands.append(literal_ids[key])
            inst=inst[:3]+operands;repaired+=1
        out.append(inst)
    words[3]=next_id;flat=words[:5]+[v for inst in out for v in inst]
    path.write_bytes(struct.pack(f"<{len(flat)}I",*flat));return repaired
