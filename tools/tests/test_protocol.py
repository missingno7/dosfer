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
    def test_paired_whitening_is_wire_data_xor(self):
        a=bytes(range(64));b=bytes((i*37)&255 for i in range(64));session=0x12345678
        left=Frame(DATA,FLAG_WHITENED,session,2,30,0,2,0,0,a)
        right=Frame(DATA,FLAG_WHITENED,session,2,31,1,2,0,0,b)
        chain=chain_frame(left,right)
        aw=left.encode()[FRAME_HEADER:];bw=right.encode()[FRAME_HEADER:];xw=chain.encode()[FRAME_HEADER:]
        self.assertEqual(xw,bytes(x^y for x,y in zip(aw,bw)))
        self.assertEqual(Frame.decode(chain.encode()).payload,bytes(x^y for x,y in zip(a,b)))
    def test_block_parity_recovers_each_unequal_member(self):
        payloads=[Record(SESSION,i+1,0,bytes([i+1])*n).encode() for i,n in enumerate((3,29,7,51,1,18,37))]
        frames=[Frame(DATA,FLAG_WHITENED,0x6A67C69D,4,80+i,i,7,0,0,p) for i,p in enumerate(payloads)]
        parity=Frame.decode(block_frame(frames).encode())
        self.assertEqual(parity.kind,BLOCK_XOR);self.assertEqual(parity.stream_id,7)
        for missing in range(7):
            members=list(payloads);members[missing]=None
            self.assertEqual(recover_block(parity,members,missing),payloads[missing])
    def test_c4_schedule_and_fixed_point_peeling(self):
        payloads=[Record(SESSION,i+1,0,bytes([i+1])*(5+i)).encode() for i in range(8)]
        frames=[Frame(DATA,FLAG_WHITENED,9,3,40+i,i,8,0,0,p) for i,p in enumerate(payloads)]
        equations=chain_blocks(frames,4)
        self.assertEqual([(e.window_index,e.stream_id) for e in equations],[(0,4),(2,4),(4,4)])
        known=list(payloads);known[1]=known[2]=None
        changed=True
        while changed:
            changed=False
            for equation in equations:
                start=equation.window_index;members=known[start:start+equation.stream_id]
                missing=[i for i,payload in enumerate(members) if payload is None]
                if len(missing)==1:
                    known[start+missing[0]]=recover_block(equation,members,missing[0]);changed=True
        self.assertEqual(known,payloads)

if __name__=="__main__": unittest.main()
