import struct, unittest
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from dosfer_protocol import *

class ProtocolTest(unittest.TestCase):
    def test_frame_roundtrip(self):
        r=Record(FILE_DATA,3,7,struct.pack(">I",12)+b"abc").encode()
        f=Frame(DATA,0,9,2,8,1,4,7,12,r)
        self.assertEqual(Frame.decode(f.encode()),f)
    def test_whitened_frame_roundtrip_and_distribution(self):
        f=Frame(DATA,FLAG_WHITENED,0x6A67C69D,0,4,4,32,1,4510,b"\0"*2283)
        wire=f.encode()[FRAME_HEADER:]
        self.assertLess(wire.count(0),30)
        self.assertEqual(wire[:16].hex(),"9d3b1923a8ac39893ead3229f43d3fee")
        self.assertEqual(Frame.decode(f.encode()),f)
    def test_checksums_reject_corruption(self):
        b=bytearray(Frame(DATA,0,1,0,0,0,1,0,0,b"x").encode()); b[-1]^=1
        with self.assertRaisesRegex(ValueError,"payload crc"): Frame.decode(bytes(b))
    def test_record_roundtrip(self):
        r=Record(FILE_END,9,2,struct.pack(">II",3,0x12345678))
        self.assertEqual(Record.decode(r.encode()),r)
    def test_paths(self):
        self.assertEqual(safe_path("A/B.TXT"),"A/B.TXT")
        for p in ("../X","/X","C:/X","A//B","A\\B",""):
            with self.assertRaises(ValueError): safe_path(p)
    def test_header_size(self):
        self.assertEqual(len(Frame(DATA,0,1,0,0,0,1,0,0,b"").encode()),48)
        self.assertEqual(len(Record(FILE_END,0,1,b"").encode()),24)
    def test_chain_peels_forward_and_backward(self):
        payloads=[Record(SESSION,i,0,bytes([i])*n).encode() for i,n in ((1,9),(2,27),(3,13))]
        frames=[Frame(DATA,FLAG_WHITENED,7,0,i,i,3,0,0,p) for i,p in enumerate(payloads)]
        ab,bc=chain_frame(frames[0],frames[1]),chain_frame(frames[1],frames[2])
        b=recover_chain(ab,payloads[0],True);self.assertEqual(recover_chain(bc,b,True),payloads[2])
        b=recover_chain(bc,payloads[2],False);self.assertEqual(recover_chain(ab,b,False),payloads[0])

if __name__=="__main__": unittest.main()
