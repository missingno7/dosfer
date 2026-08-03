from __future__ import annotations
import argparse, json, struct, sys
from pathlib import Path
from dosfer_protocol import *

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "test_vectors"

def hx(b: bytes) -> str: return b.hex()

def generate():
    session = 0x38601234
    records = [
        Record(SESSION, 0, 0, struct.pack(">IH", 0x5F3759DF, 4) + b"ROOT"),
        Record(FILE_BEGIN, 1, 1, metadata(0x20, 0x5861, 0x7C00, "HELLO.TXT", 13)),
        Record(FILE_DATA, 2, 1, struct.pack(">I", 0) + b"Hello, DOS!\r\n"),
        Record(FILE_END, 3, 1, struct.pack(">II", 13, crc32(b"Hello, DOS!\r\n"))),
        Record(TRANSFER_END, 4, 0, struct.pack(">IIII", 1, 0, 0, 13)),
    ]
    frames=[]
    for i, rec in enumerate(records):
        payload=rec.encode()
        f=Frame(DATA, 0, session, 0, i, i, len(records), rec.file_id,
                (struct.unpack(">I",rec.body[:4])[0] if rec.type==FILE_DATA else rec.record_id), payload)
        frames.append(f.encode())
    corrupt=bytearray(frames[2]); corrupt[-1]^=1
    unsupported=bytearray(frames[0]); unsupported[4]=2; unsupported[40:44]=b"\0"*4
    unsupported[40:44]=struct.pack(">I",crc32(bytes(unsupported[:48])))
    doc={
      "description":"Deterministic DOSfer v1 single-file transfer; all integers big-endian",
      "sessionId":"38601234", "fileBytesHex":hx(b"Hello, DOS!\r\n"),
      "framesHex":[hx(x) for x in frames],
      "duplicateSequence":[0,1,1,2,3,4], "missingSequence":[0,1,3,4],
      "corruptedFrameHex":hx(corrupt), "unsupportedVersionFrameHex":hx(unsupported),
      "interruptedSequence":[0,1], "resumedSequence":[1,2,3,4]
    }
    directory=[
      Record(SESSION,0,0,struct.pack(">IH",1,4)+b"TREE"),
      Record(DIRECTORY,1,0,metadata(0x10,0x5861,0x7C00,"DOCS")),
      Record(FILE_BEGIN,2,1,metadata(0x20,0x5861,0x7C00,"DOCS/README.TXT",0)),
      Record(FILE_END,3,1,struct.pack(">II",0,0)),
      Record(TRANSFER_END,4,0,struct.pack(">IIII",1,1,0,0))]
    doc2={"description":"Directory and empty file records","recordsHex":[hx(r.encode()) for r in directory]}
    return {"single_file.json":doc,"directory.json":doc2}

def main():
    ap=argparse.ArgumentParser(); ap.add_argument("--check",action="store_true"); a=ap.parse_args()
    expected=generate(); bad=False
    for name,data in expected.items():
        rendered=json.dumps(data,indent=2,sort_keys=True)+"\n"; path=OUT/name
        if a.check:
            if not path.exists() or path.read_text()!=rendered: print(f"mismatch: {path}"); bad=True
        else:
            OUT.mkdir(exist_ok=True); path.write_text(rendered)
    if bad: return 1
    print(("verified" if a.check else "generated")+f" {len(expected)} vector files")
    return 0
if __name__=="__main__": sys.exit(main())

