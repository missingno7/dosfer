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
    def test_window_128_frame_indices_roundtrip(self):
        first=Frame(DATA,FLAG_WHITENED,7,3,0,0,128,0,0,b"x")
        last=Frame(DATA,FLAG_WHITENED,7,3,127,127,128,0,0,b"y")
        self.assertEqual(Frame.decode(first.encode()).window_count,128)
        self.assertEqual(Frame.decode(last.encode()).window_index,127)
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

    def test_rgb3_stride3_whitening_fixed_sequence(self):
        parity=Frame(BLOCK_XOR,FLAG_GROUP_XOR_WHITENED,0x6A67C69D,0,120,0,66,3,3,b"\0"*16)
        self.assertEqual(parity.encode()[FRAME_HEADER:].hex(),"3c7276a924f50981b274084a5dc95cd7")
        self.assertEqual(Frame.decode(parity.encode()),parity)

    def test_rgb3_stride3_wire_payload_is_xor_of_data_wires(self):
        session=0x6A67C69D
        payloads=[Record(SESSION,i+1,0,bytes([i+1])*24).encode() for i in range(3)]
        frames=[Frame(DATA,FLAG_WHITENED,session,7,120+i*3,i*3,66,i+1,0,payloads[i]) for i in range(3)]
        parity=group_block_frame(frames,3)
        data_wires=[frame.encode()[FRAME_HEADER:] for frame in frames]
        parity_wire=parity.encode()[FRAME_HEADER:]
        self.assertEqual(parity_wire,bytes(a^b^c for a,b,c in zip(*data_wires)))
        self.assertEqual(Frame.decode(parity.encode()).payload,bytes(a^b^c for a,b,c in zip(payloads[0],payloads[1],payloads[2])))

    def test_rgb3_stride3_recovers_lost_physical_image(self):
        session=0x6A67C69D
        payloads=[Record(SESSION,i+1,0,bytes([i+1])*(11+i)).encode() for i in range(9)]
        frames=[Frame(DATA,FLAG_WHITENED,session,9,120+i,i,9,i+1,0,payloads[i]) for i in range(9)]
        equations=[Frame.decode(group_block_frame([frames[channel],frames[channel+3],frames[channel+6]],3).encode()) for channel in range(3)]
        recovered=list(payloads)
        recovered[3]=recovered[4]=recovered[5]=None
        for channel,equation in enumerate(equations):
            indices=[channel,channel+3,channel+6]
            members=[recovered[index] for index in indices]
            recovered[indices[1]]=recover_block(equation,members,1)
        self.assertEqual(recovered,payloads)

    def test_rgb3_tail_equations_support_one_or_two_members(self):
        session=0x12345678
        payloads=[Record(FILE_DATA,i+1,7,struct.pack(">I",i*10)+bytes([i+1])*(3+i)).encode() for i in range(2)]
        frames=[Frame(DATA,FLAG_WHITENED,session,4,60+i*3,i*3,8,0,0,payloads[i]) for i in range(2)]
        one=Frame.decode(group_block_frame(frames[:1],3).encode())
        two=Frame.decode(group_block_frame(frames,3).encode())
        self.assertEqual(recover_block(one,[None],0),payloads[0])
        self.assertEqual(recover_block(two,[payloads[0],None],1),payloads[1])

    def test_rgb3_group_outside_window_is_rejected(self):
        with self.assertRaisesRegex(ValueError,"group XOR"):
            Frame(BLOCK_XOR,FLAG_GROUP_XOR_WHITENED,1,0,10,4,6,2,3,b"x").encode()

if __name__=="__main__": unittest.main()
