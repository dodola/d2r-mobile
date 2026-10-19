"""Prepare a bounded shader retranslation from observed NVN vertex type facts.

Input logs contain program identities and actual NVN binding types emitted by
the Android backend. No game code is patched and no payload is uploaded here.
Different numeric layouts for the same program/location require cache variants
and are rejected rather than silently overwriting the earlier fact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

PATTERN = re.compile(r"VERTEX_TYPE_MISMATCH shader=([0-9a-f]{16}) location=(\d+) shader_kind=[012] format=(\d+) required_kind=([012])")


def observed(logs):
    facts = {}
    for text in logs:
        for key,loc,fmt,kind in PATTERN.findall(text):
            if int(loc)>=16 or int(fmt)>255 or not int(fmt):raise ValueError("invalid observed vertex slot/format")
            value=("float","sint","uint")[int(kind)]
            slots=facts.setdefault(key,{})
            if loc in slots and slots[loc]!=value:raise ValueError("conflicting vertex layouts require separate cache variants")
            slots[loc]=value
    if not facts:raise ValueError("no observed numeric mismatch in supplied logs")
    return facts


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--inventory",required=True,type=Path)
    p.add_argument("--log",required=True,type=Path,action="append")
    p.add_argument("--out",required=True,type=Path)
    a=p.parse_args();out=a.out.resolve();repo=Path(__file__).resolve().parents[2]
    if out.is_relative_to(repo) or out.exists() and any(out.iterdir()):p.error("use a fresh/empty directory outside the checkout")
    facts=observed([x.read_text(encoding="utf-8-sig") for x in a.log])
    inventory=json.loads((a.inventory/"inventory.json").read_text(encoding="utf8"))
    jobs=json.loads((a.inventory/"jobs.json").read_text(encoding="utf8"))
    selected=[]
    for job in jobs:
        if job["key"] not in facts:continue
        if job["stage"]!="vs":raise ValueError("observed program is not a vertex shader")
        job["attr"]={**job.get("attr",{}),**facts[job["key"]]};selected.append(job)
    if len(selected)!=len(facts):raise ValueError("observed program identity is absent from the original inventory")
    rows=[r for r in inventory["records"] if r["key"] in facts]
    for row in rows:
        data=(a.inventory/(row["key"]+".control.bin")).read_bytes()
        if hashlib.sha256(data).hexdigest()!=row["control_sha256"]:raise ValueError("original control identity mismatch")
    out.mkdir(parents=True)
    for key in facts:shutil.copyfile(a.inventory/(key+".control.bin"),out/(key+".control.bin"))
    inventory.update(records=rows,unique_programs=len(selected),runtime_rendering_verified=False)
    evidence={"facts":facts,"logs":[{"path":str(x.resolve()),"sha256":hashlib.sha256(x.read_bytes()).hexdigest()} for x in a.log]}
    for name,data in [("inventory.json",inventory),("jobs.json",selected),("vertex-facts.json",evidence)]:
        (out/name).write_text(json.dumps(data,indent=2)+"\n",encoding="utf8")
    print(json.dumps({"programs":len(selected),"facts":facts}))


if __name__=="__main__":main()
