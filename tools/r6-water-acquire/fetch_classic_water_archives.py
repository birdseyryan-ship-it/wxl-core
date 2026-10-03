#!/usr/bin/env python3
import concurrent.futures, hashlib, json, os, sys, urllib.request, urllib.error
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from fetch_classic_water import ROWS, decode_blte, md5, sha256

ROOT=Path("r6-classic-water-archive-fallback")
INDEX_SOURCES=[
 "https://archive.wow.tools/tpr/wow/data",
 "https://casc.wago.tools/tpr/wow/data",
 "https://us.cdn.blizzard.com/tpr/wow/data",
]
ARCHIVE_SOURCES=[
 "https://archive.wow.tools/tpr/wow/data",
 "https://casc.wago.tools/tpr/wow/data",
 "https://us.cdn.blizzard.com/tpr/wow/data",
]
TIMEOUT=12
CDN_CONFIG="c54b41b3195b9482ce0d3c6bf0b86cdb"
CONFIG_SOURCES=[
 "https://archive.wow.tools/tpr/wow/config",
 "https://casc.wago.tools/tpr/wow/config",
 "https://us.cdn.blizzard.com/tpr/wow/config",
]

def request(url,headers=None,max_bytes=None):
    req=urllib.request.Request(url,headers={"User-Agent":"Azeroth-Ironman-R6-Classic-Water-Archive/1",**(headers or {})})
    with urllib.request.urlopen(req,timeout=TIMEOUT) as r:
        status=getattr(r,"status",200); hdr=dict(r.headers.items())
        data=r.read() if max_bytes is None else r.read(max_bytes)
    return status,hdr,data

def index_url(base,h):
    return f"{base}/{h[:2]}/{h[2:4]}/{h}.index"
def archive_url(base,h):
    return f"{base}/{h[:2]}/{h[2:4]}/{h}"

def fetch_pinned_archives():
    errors=[]
    suffix=f"{CDN_CONFIG[:2]}/{CDN_CONFIG[2:4]}/{CDN_CONFIG}"
    for base in CONFIG_SOURCES:
        url=f"{base}/{suffix}"
        try:
            status,hdr,data=request(url)
            digest=hashlib.md5(data).hexdigest()
            if status!=200:
                errors.append(f"{url}: status={status}"); continue
            if digest!=CDN_CONFIG:
                errors.append(f"{url}: MD5={digest} expected={CDN_CONFIG}"); continue
            text=data.decode("utf-8")
            line=next((x for x in text.splitlines() if x.startswith("archives = ")),None)
            if not line:
                errors.append(f"{url}: archives line absent"); continue
            archives=line.split("=",1)[1].split()
            if len(archives)!=606 or len(set(archives))!=606 or any(len(x)!=32 for x in archives):
                errors.append(f"{url}: invalid archive set count={len(archives)} unique={len(set(archives))}"); continue
            return archives,{"source_url":url,"bytes":len(data),"md5":digest,"sha256":sha256(data),"count":len(archives)},errors
        except Exception as e:
            errors.append(f"{url}: {type(e).__name__}: {e}")
    raise RuntimeError("pinned CDN config unavailable or invalid: "+" | ".join(errors))

def fetch_index(h):
    errs=[]
    for base in INDEX_SOURCES:
        url=index_url(base,h)
        try:
            status,hdr,data=request(url)
            if status==200 and len(data)>=24:
                return h,url,data,errs
            errs.append(f"{url}: status={status} bytes={len(data)}")
        except Exception as e:
            errs.append(f"{url}: {type(e).__name__}: {e}")
    return h,None,None,errs

def candidate_entries(data,target):
    key=bytes.fromhex(target)
    out=[]; pos=0
    while True:
        pos=data.find(key,pos)
        if pos<0: break
        page=pos%4096
        if page<=4072 and page%24==0 and pos+24<=len(data):
            size=int.from_bytes(data[pos+16:pos+20],"big")
            off=int.from_bytes(data[pos+20:pos+24],"big")
            if 0<size<64*1024*1024:
                out.append((pos,size,off))
        pos+=1
    return out

def fetch_range(archive_hash,offset,size):
    errs=[]; end=offset+size-1
    for base in ARCHIVE_SOURCES:
        url=archive_url(base,archive_hash)
        try:
            status,hdr,data=request(url,{"Range":f"bytes={offset}-{end}"},size+1)
            cr=hdr.get("Content-Range") or hdr.get("content-range")
            ranged=(status==206 and cr and cr.startswith(f"bytes {offset}-"))
            if ranged and len(data)==size:
                return url,data,errs,cr
            errs.append(f"{url}: status={status} content-range={cr!r} bytes={len(data)} expected={size}")
        except Exception as e:
            errs.append(f"{url}: {type(e).__name__}: {e}")
    return None,None,errs,None

def main():
    ROOT.mkdir(parents=True,exist_ok=True)
    rawdir=ROOT/"encoded"; decdir=ROOT/"decoded"; rawdir.mkdir(exist_ok=True);decdir.mkdir(exist_ok=True)
    archives,config_authority,config_errors=fetch_pinned_archives()
    targets={ekey:(path,ckey) for path,ckey,ekey in ROWS}
    pending=set(targets)
    locations={}
    index_meta=[]
    print(f"Scanning {len(archives)} pinned archive indices for {len(pending)} exact EKeys",flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=32) as ex:
        futures={ex.submit(fetch_index,h):h for h in archives}
        done=0
        for fut in concurrent.futures.as_completed(futures):
            h,url,data,errs=fut.result(); done+=1
            row={"archive":h,"source_url":url,"errors":errs}
            if data is not None:
                row.update(bytes=len(data),sha256=sha256(data))
                for ekey in list(pending):
                    hits=candidate_entries(data,ekey)
                    if hits:
                        if len(hits)!=1:
                            row.setdefault("ambiguous",{})[ekey]=hits
                        else:
                            pos,size,off=hits[0]
                            locations[ekey]={"archive":h,"index_url":url,"index_sha256":sha256(data),"index_position":pos,"offset":off,"size":size}
                            pending.remove(ekey)
                            print(f"LOCATED {targets[ekey][0]} archive={h} offset={off} size={size}",flush=True)
            index_meta.append(row)
            if done%50==0: print(f"indices {done}/{len(archives)} pending={len(pending)}",flush=True)
    results=[]
    for ekey,(path,ckey) in targets.items():
        rec={"path":path,"ckey":ckey,"ekey":ekey}
        loc=locations.get(ekey)
        if not loc:
            rec.update(status="unresolved_in_archive_indices");results.append(rec);continue
        rec["location"]=loc
        url,blob,errs,cr=fetch_range(loc["archive"],loc["offset"],loc["size"])
        rec["range_errors"]=errs
        if blob is None:
            rec["status"]="archive_range_fetch_failed";results.append(rec);continue
        rec.update(archive_url=url,content_range=cr,encoded_bytes=len(blob),encoded_sha256=sha256(blob))
        stem=path.replace("/","__"); (rawdir/f"{stem}.blte").write_bytes(blob)
        try:
            decoded=decode_blte(blob)
            rec.update(decoded_bytes=len(decoded),decoded_md5=md5(decoded),decoded_sha256=sha256(decoded),ckey_match=md5(decoded)==ckey,
                       gxsh_count=decoded.count(b"GXSH"),hsxg_count=decoded.count(b"HSXG"),dxbc_signature_count=decoded.count(b"DXBC"))
            if not rec["ckey_match"]: raise ValueError(f"CKey mismatch {rec['decoded_md5']} != {ckey}")
            (decdir/f"{stem}.decoded").write_bytes(decoded)
            rec["status"]="verified"
        except Exception as e:
            rec["status"]="decode_or_ckey_failed";rec["error"]=f"{type(e).__name__}: {e}"
        results.append(rec)
    summary={"archive_count":len(archives),"target_count":len(ROWS),"locations":len(locations),
             "verified":sum(r["status"]=="verified" for r in results),
             "unresolved":sum(r["status"]=="unresolved_in_archive_indices" for r in results),
             "range_failures":sum(r["status"]=="archive_range_fetch_failed" for r in results),
             "decode_failures":sum(r["status"]=="decode_or_ckey_failed" for r in results),
             "results":results,
             "cdn_config":CDN_CONFIG,"config_authority":config_authority,"config_prior_errors":config_errors,
             "index_sources":INDEX_SOURCES,"archive_sources":ARCHIVE_SOURCES}
    (ROOT/"ARCHIVE_FALLBACK.json").write_text(json.dumps(summary,indent=2)+"\n")
    lines=[f"archives={summary['archive_count']} targets={summary['target_count']} located={summary['locations']} verified={summary['verified']} unresolved={summary['unresolved']} range_failures={summary['range_failures']} decode_failures={summary['decode_failures']}"]
    for r in results:
        lines.append(f"{r['status']} {r['path']} archive={r.get('location',{}).get('archive','')} offset={r.get('location',{}).get('offset','')} size={r.get('location',{}).get('size','')} ckey_match={r.get('ckey_match')}")
        if r.get("error"): lines.append("  "+r["error"])
    (ROOT/"ARCHIVE_FALLBACK_SUMMARY.txt").write_text("\n".join(lines)+"\n")
    print("\n".join(lines))
    # Proven CKey mismatch is a hard failure. Absence/range unavailability remains useful evidence.
    return 2 if any(r.get("ckey_match") is False for r in results) else 0

if __name__=="__main__": sys.exit(main())
