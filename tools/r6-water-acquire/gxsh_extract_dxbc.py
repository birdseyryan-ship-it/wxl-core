#!/usr/bin/env python3
import argparse, csv, hashlib, json, struct, zlib
from pathlib import Path

def u32(b,o): return struct.unpack_from("<I",b,o)[0]
def sha256(b): return hashlib.sha256(b).hexdigest()

def inflate_stream(blob,start):
    d=zlib.decompressobj(); out=d.decompress(blob[start:]); out+=d.flush()
    if not d.eof: raise ValueError("zlib stream did not reach EOF")
    used=len(blob[start:])-len(d.unused_data)
    return out,used

def find_dxbc(record):
    out=[]; pos=0
    while True:
        i=record.find(b"DXBC",pos)
        if i<0: return out
        pos=i+4
        if i+32>len(record): continue
        size=u32(record,i+24); chunks=u32(record,i+28)
        if size<32 or i+size>len(record) or chunks>256 or 32+4*chunks>size: continue
        good=True
        for n in range(chunks):
            co=u32(record,i+32+4*n)
            if co+8>size: good=False; break
            clen=u32(record,i+co+4)
            if co+8+clen>size: good=False; break
        if good: out.append((i,size,record[i:i+size]))

def parse(path,outroot):
    b=path.read_bytes()
    if b[:4]!=b"HSXG" or u32(b,4)!=0x00010006: raise ValueError("unexpected GXSH")
    n=u32(b,12); ofs_chunks=u32(b,16); nchunks=u32(b,20); ofs_data=u32(b,24)
    offsets=[u32(b,28+4*i) for i in range(n)]
    chunks=[u32(b,ofs_chunks+4*i) for i in range(nchunks)]
    payload=b"".join(inflate_stream(b,ofs_data+c)[0] for c in chunks)
    valid=[(i,o) for i,o in enumerate(offsets) if o!=0xffffffff]
    distinct=sorted(set(o for _,o in valid))
    ends={o:(distinct[i+1] if i+1<len(distinct) else len(payload)) for i,o in enumerate(distinct)}
    family=path.name.rsplit(".bls",1)[0]
    outdir=outroot/family;outdir.mkdir(parents=True,exist_ok=True)
    rows=[]
    for slot,off in valid:
        rec=payload[off:ends[off]]
        hits=find_dxbc(rec)
        if len(hits)!=1: raise ValueError(f"{family} slot {slot}: expected 1 DXBC, got {len(hits)}")
        do,ds,dx=hits[0]
        target=outdir/f"slot_{slot:04d}.dxbc"; target.write_bytes(dx)
        rows.append({"family":family,"slot":slot,"record_offset":off,"record_bytes":len(rec),
                     "record_sha256":sha256(rec),"dxbc_offset":do,"dxbc_bytes":ds,
                     "dxbc_sha256":sha256(dx),"dxbc_file":str(target).replace("\\","/")})
    return rows

def main():
    ap=argparse.ArgumentParser();ap.add_argument("input",type=Path);ap.add_argument("output",type=Path)
    a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    rows=[]
    for p in sorted(a.input.glob("*.decoded")):
        # Keep .bls in family name, strip only .decoded.
        q=a.output.parent/(p.name[:-8])
        # parse uses file name only and accepts any suffix
        rows.extend(parse(p,a.output))
    m=a.output.parent/"DXBC_MANIFEST.json";m.write_text(json.dumps(rows,indent=2)+"\n")
    with (a.output.parent/"DXBC_MANIFEST.tsv").open("w",newline="",encoding="utf-8") as f:
        w=csv.DictWriter(f,fieldnames=rows[0].keys(),delimiter="\t");w.writeheader();w.writerows(rows)
    unique=len({r["dxbc_sha256"] for r in rows})
    families=len({r["family"] for r in rows})
    summary=f"families={families} slots={len(rows)} unique_dxbc={unique}\n"
    (a.output.parent/"DXBC_SUMMARY.txt").write_text(summary);print(summary,end="")
if __name__=="__main__": main()
