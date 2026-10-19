"""Run the original Echoes SDK's synthetic AudioRenderer regression on Android.

Verifies prepared module hashes, does not launch/restart either game APK, and
stores receipts outside the checkout. Requires an explicitly selected device.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid

REPO = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir",type=Path,required=True)
    p.add_argument("--prepared",type=Path,required=True)
    p.add_argument("--out",type=Path,required=True)
    p.add_argument("--serial",required=True)
    p.add_argument("--adb",default="adb")
    p.add_argument("--cmake",default="cmake")
    a=p.parse_args();out=a.out.resolve()
    if out.is_relative_to(REPO) or out.exists() and any(out.iterdir()):
        p.error("use a fresh/empty output directory outside the checkout")
    audit=json.loads((a.prepared/"title-audit.json").read_text(encoding="utf8"))
    if audit.get("title")!="echoes-of-wisdom" or audit.get("content_version")!=196608 or audit.get("rewrite_target")!="android" or not audit.get("rewritten"):
        p.error("expected the pinned rewritten Echoes 1.1.0 Android module set")
    products=audit["products"]
    if len(products)!=28 or any(Path(n).name!=n for n in products):
        p.error("invalid module product manifest")
    for name,record in products.items():
        data=(a.prepared/name).read_bytes()
        if len(data)!=record["bytes"] or hashlib.sha256(data).hexdigest()!=record["sha256"]:
            p.error("prepared product identity mismatch: "+name)
    out.mkdir(parents=True,exist_ok=True)

    def run(command,label,timeout=60):
        r=subprocess.run(list(map(str,command)),capture_output=True,timeout=timeout)
        (out/(label+".stdout.txt")).write_bytes(r.stdout)
        (out/(label+".stderr.txt")).write_bytes(r.stderr)
        if r.returncode:raise RuntimeError(f"{label} failed ({r.returncode}); see {out}")
        return r.stdout

    run([a.cmake,"--build",a.build_dir,"--target","dm_android_ar_probe","-j6"],"build",180)
    binary=a.build_dir/"dm_android_ar_probe";digest=hashlib.sha256(binary.read_bytes()).hexdigest()
    adb=[a.adb,"-s",a.serial];remote="/data/local/tmp/echoes-ar-check-"+uuid.uuid4().hex[:12]
    run([*adb,"shell","mkdir","-p",remote+"/modules"],"mkdir")
    run([*adb,"push",binary,remote+"/probe"],"push-probe")
    for name in products:run([*adb,"push",a.prepared/name,remote+"/modules/"+name],"push-"+name)
    manifest=out/"modules.sha256"
    manifest.write_bytes("".join(r["sha256"]+"  "+remote+"/modules/"+n+"\n" for n,r in products.items()).encode("ascii"))
    run([*adb,"push",manifest,remote+"/modules.sha256"],"push-manifest")
    checked=run([*adb,"shell","sha256sum","-c",remote+"/modules.sha256"],"verify-modules").decode().splitlines()
    if len(checked)!=28 or not all(x.endswith(": OK") for x in checked):raise RuntimeError("device module verification incomplete")
    run([*adb,"shell","chmod","700",remote+"/probe"],"chmod")
    run([*adb,"shell","timeout","25",remote+"/probe","--title-profile","echoes-of-wisdom-1.1.0",remote+"/modules",remote+"/result.json","audio-renderer",remote+"/fixture"],"probe",35)
    result=json.loads(run([*adb,"shell","cat",remote+"/result.json"],"read-result"))
    if not result.get("passed") or result.get("cycles")!=3 or result.get("open_renderers")!=0 or not result.get("device_read_frames"):
        raise RuntimeError("incomplete AudioRenderer fixture receipt")
    result.update(serial=a.serial,binary_sha256=digest,prepared_modules_sha256_verified=True,prepared_audit_sha256=hashlib.sha256((a.prepared/"title-audit.json").read_bytes()).hexdigest())
    (out/"result.json").write_text(json.dumps(result,indent=2)+"\n",encoding="utf8")
    print(json.dumps(result))


if __name__=="__main__":main()
