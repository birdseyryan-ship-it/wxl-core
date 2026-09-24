#!/usr/bin/env python3
import argparse, ctypes, hashlib, json, os, platform
from pathlib import Path

def sha256(b): return hashlib.sha256(b).hexdigest()

def disassemble(data):
    if os.name!="nt": raise RuntimeError("D3DDisassemble requires Windows")
    dll=ctypes.WinDLL("d3dcompiler_47.dll")
    fn=dll.D3DDisassemble
    fn.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint,ctypes.c_char_p,ctypes.POINTER(ctypes.c_void_p)]
    fn.restype=ctypes.c_long
    src=ctypes.create_string_buffer(data); blob=ctypes.c_void_p()
    hr=fn(src,len(data),0,None,ctypes.byref(blob))
    if hr<0 or not blob.value: raise RuntimeError(f"D3DDisassemble failed HRESULT 0x{ctypes.c_uint32(hr).value:08X}")
    vtbl=ctypes.cast(blob,ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    GetPtr=ctypes.WINFUNCTYPE(ctypes.c_void_p,ctypes.c_void_p)(vtbl[3])
    GetSize=ctypes.WINFUNCTYPE(ctypes.c_size_t,ctypes.c_void_p)(vtbl[4])
    Release=ctypes.WINFUNCTYPE(ctypes.c_ulong,ctypes.c_void_p)(vtbl[2])
    try:
        p=GetPtr(blob); n=GetSize(blob); return ctypes.string_at(p,n)
    finally: Release(blob)

def main():
    ap=argparse.ArgumentParser();ap.add_argument("root",type=Path)
    a=ap.parse_args(); rows=[]
    for p in sorted(a.root.rglob("*.dxbc")):
        b=p.read_bytes(); text=disassemble(b)
        out=p.with_suffix(".asm");out.write_bytes(text)
        rows.append({"dxbc_file":str(p).replace("\\","/"),"dxbc_sha256":sha256(b),"bytes":len(b),
                     "asm_file":str(out).replace("\\","/"),"asm_sha256":sha256(text),"asm_bytes":len(text)})
        print(f"PASS {p} {len(b)} -> {len(text)}")
    (a.root.parent/"DISASSEMBLY_MANIFEST.json").write_text(json.dumps(rows,indent=2)+"\n")
    families={}
    for r in rows:
        fam=Path(r["dxbc_file"]).parent.name;families[fam]=families.get(fam,0)+1
    lines=[f"dxbc={len(rows)} families={len(families)} platform={platform.platform()} d3dcompiler=d3dcompiler_47.dll"]
    lines += [f"{k}: {v}" for k,v in sorted(families.items())]
    (a.root.parent/"DISASSEMBLY_SUMMARY.txt").write_text("\n".join(lines)+"\n")
    print("\n".join(lines))
if __name__=="__main__": main()
