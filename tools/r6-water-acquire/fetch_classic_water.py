#!/usr/bin/env python3
import hashlib, json, struct, sys, time, urllib.request, urllib.error, zlib
from pathlib import Path

BUILD = "1.13.2.31650"
BUILD_CONFIG = "2c915a9a226a3f35af6c65fcc7b6ca4a"
CDN_CONFIG = "c54b41b3195b9482ce0d3c6bf0b86cdb"

ROWS = [
("shaders/pixel/ps_5_0/ffxunderwaterhigh.bls","94820b8a57950214c98ac96790358b98","46ba82c726be75d4aee7351e42925c2f"),
("shaders/pixel/ps_5_0/ffxunderwaterlow.bls","0863e0688de010df43ee2e8c748ffa27","9b0ecc72ce9df42eeee097391cbad8df"),
("shaders/pixel/ps_5_0/ffxwaterwindow.bls","afd2b39d016d1c9f5a0a81bde9d23ba5","69983e140161a2c1bd993b8c0f0b7187"),
("shaders/pixel/ps_5_0/mediumwater.bls","1f9fd24b3b03b71c5f66331f91591035","12d786a7ccc499e85fb3272f5fadd2d8"),
("shaders/pixel/ps_5_0/mediumwaterbelow.bls","a5878470a4cdd97352ffd712a80bab90","bfd3901234980362bd1f481bc67ea5fa"),
("shaders/pixel/ps_5_0/procwaterabove.bls","9b25cc385b52b1c7837b5cce53682ae8","cf7909cc30d7c80c0e5f2661c00dc5ca"),
("shaders/pixel/ps_5_0/procwaterabovefog.bls","179a949c69f7cd39fba050f77d20c39b","801aeb941aad58fa7193b459e9d159f1"),
("shaders/pixel/ps_5_0/procwaterbelow.bls","1350c48157a789b0b9846568a5c2e138","35d477aed1546e19b71bbbad066046f7"),
("shaders/pixel/ps_5_0/refraction.bls","b6b8594d8c950a03fe058785c3c7eb61","7e2e7b99019cb9d9f00245664543a2e4"),
("shaders/pixel/ps_5_0/refractionapply.bls","dbd4c8110ce8d0394dd32980786396fa","712477dd6be2bde291fb6946433dae69"),
("shaders/pixel/ps_5_0/water.bls","d6af3564024c6fdaed7049b3177e79ff","4ad4aa7854e40c0c6d1c14c0d047285e"),
("shaders/pixel/ps_5_0/waterfogpoly.bls","ac80e0bda5a6d8ea5b54c391a3efb9c6","319d888e0b3f50aa1c9a75332442c5d2"),
("shaders/pixel/ps_5_0/waterripples.bls","9ea8f21194ca02c46bb4662774fac888","263078dd927d1b510b1ab0bed826b19e"),
("shaders/vertex/vs_5_0/ffxwaterwindow.bls","e754e84e3d6b7e27c678cf3f557eec9f","f22846b6f6557643687df11f7e966f1b"),
("shaders/vertex/vs_5_0/procwater.bls","b3125b5add38dc882b2f0c9e43232d20","ec3c4500999db5ed17d1fe0bb746e405"),
("shaders/vertex/vs_5_0/water.bls","a162a0f66a4cbafddb755543895ab826","e83016d60563a94288522674e9019156"),
("shaders/vertex/vs_5_0/waterfogpoly.bls","f14749239deb1ec7bb0b9a3a4cb0bbb8","256d424a535950b0ef4c3d3a8331401a"),
("shaders/vertex/vs_5_0/waterripples.bls","8b2c7f1a96926c53ef8ac68b847b7bb0","cc5360e23597793fdbb87e807aab8ece"),
]
BASES = [
 "https://level3.ssl.blizzard.com/tpr/wow/data",
 "https://us.cdn.blizzard.com/tpr/wow/data",
 "https://archive.wow.tools/tpr/wow/data",
 "https://casc.wago.tools/tpr/wow/data",
 "http://cdn.arctium.tools/tpr/wow/data",
]

def md5(b): return hashlib.md5(b).hexdigest()
def sha256(b): return hashlib.sha256(b).hexdigest()

def decode_chunk(body):
    if not body: raise ValueError("empty BLTE chunk")
    mode,payload=body[:1],body[1:]
    if mode==b'N': return payload
    if mode==b'Z': return zlib.decompress(payload)
    if mode==b'F': return decode_blte(payload)
    if mode==b'E': raise ValueError("encrypted BLTE chunk (TACT key required)")
    raise ValueError(f"unsupported BLTE chunk mode {mode!r}")

def decode_blte(raw):
    if raw[:4]!=b'BLTE':
        if len(raw)>=34 and raw[30:34]==b'BLTE': raw=raw[30:]
        else: raise ValueError("object is not BLTE")
    if len(raw)<8: raise ValueError("short BLTE")
    header=struct.unpack(">I",raw[4:8])[0]
    if header==0: return decode_chunk(raw[8:])
    if header<12 or header>len(raw): raise ValueError("invalid BLTE header size")
    count=int.from_bytes(raw[9:12],"big")
    table_end=12+24*count
    if table_end>header or table_end>len(raw): raise ValueError("invalid BLTE chunk table")
    entries=[]; p=12
    for _ in range(count):
        csz,dsz=struct.unpack(">II",raw[p:p+8]); digest=raw[p+8:p+24]; p+=24
        entries.append((csz,dsz,digest))
    pos=header; out=bytearray()
    for idx,(csz,dsz,digest) in enumerate(entries):
        body=raw[pos:pos+csz]; pos+=csz
        if len(body)!=csz: raise ValueError(f"short BLTE chunk {idx}")
        if hashlib.md5(body).digest()!=digest: raise ValueError(f"BLTE chunk MD5 mismatch {idx}")
        dec=decode_chunk(body)
        if len(dec)!=dsz: raise ValueError(f"BLTE chunk size mismatch {idx}: {len(dec)} != {dsz}")
        out.extend(dec)
    return bytes(out)

def fetch(ekey):
    suffix=f"{ekey[:2]}/{ekey[2:4]}/{ekey}"
    errors=[]
    for base in BASES:
        url=f"{base}/{suffix}"
        try:
            req=urllib.request.Request(url,headers={"User-Agent":"Azeroth-Ironman-R6-Classic-Water-Acquisition/1"})
            with urllib.request.urlopen(req,timeout=30) as r:
                data=r.read()
            if data:
                return data,url,errors
            errors.append(f"{url}: empty")
        except Exception as e:
            errors.append(f"{url}: {type(e).__name__}: {e}")
    raise RuntimeError(" | ".join(errors))

def extract_dxbc(data,outdir,stem):
    found=[]; start=0; idx=0
    while True:
        off=data.find(b"DXBC",start)
        if off<0: break
        start=off+4
        if off+32>len(data): continue
        size=struct.unpack_from("<I",data,off+24)[0]
        chunks=struct.unpack_from("<I",data,off+28)[0]
        if size<32 or off+size>len(data) or chunks>256: continue
        blob=data[off:off+size]
        # Basic chunk-offset sanity.
        if 32+4*chunks>len(blob): continue
        good=True
        for i in range(chunks):
            co=struct.unpack_from("<I",blob,32+4*i)[0]
            if co+8>len(blob): good=False; break
            clen=struct.unpack_from("<I",blob,co+4)[0]
            if co+8+clen>len(blob): good=False; break
        if not good: continue
        path=outdir/f"{stem}.dxbc{idx:03d}.bin"; path.write_bytes(blob)
        found.append({"offset":off,"size":size,"chunks":chunks,"sha256":sha256(blob),"file":path.name})
        idx+=1; start=off+size
    return found

def main():
    root=Path("r6-classic-water-acquire"); rawdir=root/"raw"; decdir=root/"decoded"; dxdir=root/"dxbc"
    for d in (rawdir,decdir,dxdir): d.mkdir(parents=True,exist_ok=True)
    results=[]; failures=0
    for n,(path,ckey,ekey) in enumerate(ROWS,1):
        rec={"path":path,"ckey":ckey,"ekey":ekey}
        stem=path.replace("/","__")
        try:
            raw,url,errors=fetch(ekey)
            rec.update(source_url=url,raw_bytes=len(raw),raw_sha256=sha256(raw),prior_fetch_errors=errors)
            (rawdir/f"{stem}.blte").write_bytes(raw)
            dec=decode_blte(raw)
            rec.update(decoded_bytes=len(dec),decoded_md5=md5(dec),decoded_sha256=sha256(dec),ckey_match=md5(dec)==ckey,
                       magic_ascii="".join(chr(x) if 32<=x<127 else "." for x in dec[:16]),
                       gxsh_count=dec.count(b"GXSH"),hsxg_count=dec.count(b"HSXG"),dxbc_signature_count=dec.count(b"DXBC"))
            if not rec["ckey_match"]: raise ValueError(f"decoded CKey mismatch: {rec['decoded_md5']} != {ckey}")
            (decdir/f"{stem}.decoded").write_bytes(dec)
            rec["dxbc"]=extract_dxbc(dec,dxdir,stem)
            rec["status"]="ok"
        except Exception as e:
            failures+=1; rec["status"]="failed";rec["error"]=f"{type(e).__name__}: {e}"
        results.append(rec)
        print(f"[{n:02d}/{len(ROWS)}] {rec['status'].upper():6} {path} {rec.get('source_url','')}",flush=True)
    summary={
      "build":BUILD,"build_config":BUILD_CONFIG,"cdn_config":CDN_CONFIG,
      "requested":len(ROWS),"succeeded":len(ROWS)-failures,"failed":failures,
      "verified_ckeys":sum(1 for r in results if r.get("ckey_match") is True),
      "dxbc_containers":sum(len(r.get("dxbc",[])) for r in results),
      "results":results,
    }
    (root/"ACQUISITION.json").write_text(json.dumps(summary,indent=2)+"\n")
    lines=[f"Classic {BUILD} targeted water shader acquisition",
           f"requested={summary['requested']} succeeded={summary['succeeded']} failed={summary['failed']} verified_ckeys={summary['verified_ckeys']} dxbc={summary['dxbc_containers']}"]
    for r in results:
        lines.append(f"{r['status']} {r['path']} ckey_match={r.get('ckey_match')} dxbc={len(r.get('dxbc',[]))} source={r.get('source_url','')}")
        if r.get("error"): lines.append("  "+r["error"])
    (root/"SUMMARY.txt").write_text("\n".join(lines)+"\n")
    print("\n".join(lines))
    # Acquisition failure is evidence, not a workflow failure; only fail if a retrieved decode mismatched its pinned CKey.
    mismatches=[r for r in results if r.get("decoded_md5") and r.get("ckey_match") is False]
    return 2 if mismatches else 0
if __name__=="__main__": sys.exit(main())
