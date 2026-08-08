/* 
 * QR Code generator library (C)
 * 
 * Copyright (c) Project Nayuki. (MIT License)
 * https://www.nayuki.io/page/qr-code-generator-library
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * - The above copyright notice and this permission notice shall be included in
 *   all copies or substantial portions of the Software.
 * - The Software is provided "as is", without warranty of any kind, express or
 *   implied, including but not limited to the warranties of merchantability,
 *   fitness for a particular purpose and noninfringement. In no event shall the
 *   authors or copyright holders be liable for any claim, damages or other
 *   liability, whether in an action of contract, tort or otherwise, arising from,
 *   out of or in connection with the Software or the use or other dealings in the
 *   Software.
 */

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef __WATCOMC__
#include <dos.h>
#endif
#include "qrcodegen.h"

#ifdef DOSFER_PROFILE
#include "timing.h"
u32 dosferQrProfileTicks[6];
#define DOSFER_PROFILE_START u32 dosferProfileNow, dosferProfileThen = timer_ticks()
#define DOSFER_PROFILE_MARK(i) do { dosferProfileNow=timer_ticks(); dosferQrProfileTicks[i]+=dosferProfileNow-dosferProfileThen; dosferProfileThen=dosferProfileNow; } while(0)
#else
#define DOSFER_PROFILE_START
#define DOSFER_PROFILE_MARK(i)
#endif

#ifndef QRCODEGEN_TEST
	#define testable static  // Keep functions private
#else
	#define testable  // Expose private functions
#endif


/*---- Forward declarations for private functions ----*/

// Regarding all public and private functions defined in this source file:
// - They require all pointer/array arguments to be not null unless the array length is zero.
// - They only read input scalar/array arguments, write to output pointer/array
//   arguments, and return scalar values; they are "pure" functions.
// - They don't read mutable global variables or write to any global variables.
// - They don't perform I/O, read the clock, print to console, etc.
// - They allocate a small and constant amount of stack memory.
// - They don't allocate or free any memory on the heap.
// - They don't recurse or mutually recurse. All the code
//   could be inlined into the top-level public functions.
// - They run in at most quadratic time with respect to input arguments.
//   Most functions run in linear time, and some in constant time.
//   There are no unbounded loops or non-obvious termination conditions.
// - They are completely thread-safe if the caller does not give the
//   same writable buffer to concurrent calls to these functions.

testable void appendBitsToBuffer(unsigned int val, int numBits, uint8_t buffer[], int *bitLen);

testable void addEccAndInterleave(uint8_t data[], int version, enum qrcodegen_Ecc ecl, uint8_t result[]);
testable int getNumDataCodewords(int version, enum qrcodegen_Ecc ecl);
testable int getNumRawDataModules(int ver);

testable void reedSolomonComputeDivisor(int degree, uint8_t result[]);
testable void reedSolomonComputeRemainder(const uint8_t data[], int dataLen,
	const uint8_t generator[], int degree, uint8_t result[]);
testable uint8_t reedSolomonMultiply(uint8_t x, uint8_t y);

testable void initializeFunctionModules(int version, uint8_t qrcode[]);
static void drawLightFunctionModules(uint8_t qrcode[], int version);
static void drawFormatBits(enum qrcodegen_Ecc ecl, enum qrcodegen_Mask mask, uint8_t qrcode[]);
testable int getAlignmentPatternPositions(int version, uint8_t result[7]);
static void fillRectangle(int left, int top, int width, int height, uint8_t qrcode[]);

static void drawCodewords(const uint8_t data[], int dataLen, uint8_t qrcode[]);
static void applyMask(const uint8_t functionModules[], uint8_t qrcode[], enum qrcodegen_Mask mask);
static bool dosferPrepareMatrixCache(int version, enum qrcodegen_Ecc ecl, enum qrcodegen_Mask mask);
static void dosferDrawCodewordsCached(const uint8_t data[], int dataLen, uint8_t qrcode[]);
static uint8_t *dosferFunctionTemplate;
/* One linear module index per codeword bit.  The old cache kept a 16-bit
 * matrix-byte index plus a separate 8-bit mask (3 bytes per bit).  A linear
 * 0..31328 module index carries the same information in 2 bytes and can be
 * handed directly to the VGA backend after the first canonical render. */
static uint16_t *dosferDataModule;
static int dosferCacheVersion;
static int dosferCacheEcl = -1;
static int dosferCacheMask = -1;
static int dosferCacheDataBits;
static size_t dosferTemplateCapacity;
static size_t dosferMapCapacity;
testable long getPenaltyScore(const uint8_t qrcode[]);
static int finderPenaltyCountPatterns(const int runHistory[7], int qrsize);
static int finderPenaltyTerminateAndCount(bool currentRunColor, int currentRunLength, int runHistory[7], int qrsize);
static void finderPenaltyAddHistory(int currentRunLength, int runHistory[7], int qrsize);

testable bool getModuleBounded(const uint8_t qrcode[], int x, int y);
testable void setModuleBounded(uint8_t qrcode[], int x, int y, bool isDark);
testable void setModuleUnbounded(uint8_t qrcode[], int x, int y, bool isDark);
static bool getBit(int x, int i);

testable int calcSegmentBitLength(enum qrcodegen_Mode mode, size_t numChars);
testable int getTotalBits(const struct qrcodegen_Segment segs[], size_t len, int version);
static int numCharCountBits(enum qrcodegen_Mode mode, int version);



/*---- Private tables of constants ----*/

// The set of all legal characters in alphanumeric mode, where each character
// value maps to the index in the string. For checking text and encoding segments.
static const char *ALPHANUMERIC_CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

// Sentinel value for use in only some functions.
#define LENGTH_OVERFLOW -1

// For generating error correction codes.
testable const int8_t ECC_CODEWORDS_PER_BLOCK[4][41] = {
	// Version: (note that index 0 is for padding, and is set to an illegal value)
	//0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40    Error correction level
	{-1,  7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // Low
	{-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},  // Medium
	{-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // Quartile
	{-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // High
};

#define qrcodegen_REED_SOLOMON_DEGREE_MAX 30  // Based on the table above
#define DOSFER_RS_STRIDE 32  /* Power-of-two rows make factor lookup one shift on a 386. */

/* The lookup table is declared before the inline kernels because the fused
 * RGB kernel addresses it directly instead of carrying a generic base pointer
 * in a register. */
static uint8_t
#ifdef __WATCOMC__
	__near
#endif
	dosferRsStep[256U * DOSFER_RS_STRIDE];

#ifdef __WATCOMC__
static uint8_t __near dosferRsEcc3[3][32];
static const uint8_t __far *dosferRsBlue3;
static uint8_t __far *dosferRsEccBlueOut3;
#endif

#ifdef __WATCOMC__
/* V40-L interleaves each block at a constant 25-byte destination stride.
 * Keeping this tiny copy loop in registers avoids two C loop tests and a
 * special-case branch for every one of its 3,706 codewords. */
static void dosferStrideCopy(const uint8_t __far *src,uint16_t len,
		uint8_t __far *dest,uint16_t stride);
#pragma aux dosferStrideCopy = \
	"test cx,cx" \
	"jz stride_done" \
	"stride_loop:" \
	"mov al,fs:[si]" \
	"mov es:[di],al" \
	"inc si" \
	"add di,dx" \
	"dec cx" \
	"jnz stride_loop" \
	"stride_done:" \
	parm [fs si] [cx] [es di] [dx] modify [ax cx si di];

/* The specialized V40-L encoder always copies 118 data bytes and 30 ECC
 * bytes at stride 25. Pairing adjacent source bytes halves loop and pointer
 * overhead while preserving the exact interleaved destination order. */
static void dosferStrideCopy118(const uint8_t __far *src,uint8_t __far *dest);
#pragma aux dosferStrideCopy118 = \
	"mov cx,14" \
	"stride118_loop:" \
	"mov al,fs:[si]"     "mov es:[di],al" \
	"mov al,fs:[si+1]"   "mov es:[di+25],al" \
	"mov al,fs:[si+2]"   "mov es:[di+50],al" \
	"mov al,fs:[si+3]"   "mov es:[di+75],al" \
	"mov al,fs:[si+4]"   "mov es:[di+100],al" \
	"mov al,fs:[si+5]"   "mov es:[di+125],al" \
	"mov al,fs:[si+6]"   "mov es:[di+150],al" \
	"mov al,fs:[si+7]"   "mov es:[di+175],al" \
	"add si,8" "add di,200" \
	"dec cx" "jnz stride118_loop" \
	"mov al,fs:[si]"     "mov es:[di],al" \
	"mov al,fs:[si+1]"   "mov es:[di+25],al" \
	"mov al,fs:[si+2]"   "mov es:[di+50],al" \
	"mov al,fs:[si+3]"   "mov es:[di+75],al" \
	"mov al,fs:[si+4]"   "mov es:[di+100],al" \
	"mov al,fs:[si+5]"   "mov es:[di+125],al" \
	parm [fs si] [es di] modify [ax cx si di];

static void dosferStrideCopy30(const uint8_t __far *src,uint8_t __far *dest);
#pragma aux dosferStrideCopy30 = \
	"mov cx,3" \
	"stride30_loop:" \
	"mov al,fs:[si]"     "mov es:[di],al" \
	"mov al,fs:[si+1]"   "mov es:[di+25],al" \
	"mov al,fs:[si+2]"   "mov es:[di+50],al" \
	"mov al,fs:[si+3]"   "mov es:[di+75],al" \
	"mov al,fs:[si+4]"   "mov es:[di+100],al" \
	"mov al,fs:[si+5]"   "mov es:[di+125],al" \
	"mov al,fs:[si+6]"   "mov es:[di+150],al" \
	"mov al,fs:[si+7]"   "mov es:[di+175],al" \
	"add si,8" "add di,200" \
	"dec cx" "jnz stride30_loop" \
	"mov al,fs:[si]"     "mov es:[di],al" \
	"mov al,fs:[si+1]"   "mov es:[di+25],al" \
	"mov al,fs:[si+2]"   "mov es:[di+50],al" \
	"mov al,fs:[si+3]"   "mov es:[di+75],al" \
	"mov al,fs:[si+4]"   "mov es:[di+100],al" \
	"mov al,fs:[si+5]"   "mov es:[di+125],al" \
	parm [fs si] [es di] modify [ax cx si di];

/* The large-model ABI keeps near static storage in DGROUP (the same segment
 * as SS).  A preceding far-memory operation may have loaded DS with its data
 * segment; restore the ABI invariant once before a complete V40 RS pass.
 * This is deliberately outside the 25-block loop: it adds two instructions
 * per full QR construction, not to the steady-state ECC path. */
static void dosferRestoreDgroup(void);
#pragma aux dosferRestoreDgroup = \
	"push ss" \
	"pop ds" \
	modify [ds];

/* The six V40-L long-block data bytes are consecutive at the destination but
 * have 119-byte source spacing.  Copy them after all RS work through explicit
 * far segments, so no C far store can invalidate DS between RS blocks. */
static void dosferCopyV40LongData(const uint8_t __far *data,uint8_t __far *result);
#pragma aux dosferCopyV40LongData = \
	"mov al,fs:[si+2360]" \
	"mov es:[di+2950],al" \
	"mov al,fs:[si+2479]" \
	"mov es:[di+2951],al" \
	"mov al,fs:[si+2598]" \
	"mov es:[di+2952],al" \
	"mov al,fs:[si+2717]" \
	"mov es:[di+2953],al" \
	"mov al,fs:[si+2836]" \
	"mov es:[di+2954],al" \
	"mov al,fs:[si+2955]" \
	"mov es:[di+2955],al" \
	parm [fs si] [es di] modify [ax si di];

/* Transpose the common 118 bytes of all 25 V40-L data blocks. Reading one
 * byte from each block makes the 25 destination bytes contiguous and cuts
 * the loop/control count from 25*15 to 118 iterations. */
static void dosferInterleaveDataV40(const uint8_t __far *data,
		uint8_t __far *result);
#pragma aux dosferInterleaveDataV40 = \
	"mov cx,118" \
	"v40data_row:" \
	"mov al,fs:[si]"      "mov ah,fs:[si+118]" \
	"ror eax,16" "mov al,fs:[si+236]" "mov ah,fs:[si+354]" \
	"rol eax,16" "mov es:[di],eax" \
	"mov al,fs:[si+472]"  "mov ah,fs:[si+590]" \
	"ror eax,16" "mov al,fs:[si+708]" "mov ah,fs:[si+826]" \
	"rol eax,16" "mov es:[di+4],eax" \
	"mov al,fs:[si+944]"  "mov ah,fs:[si+1062]" \
	"ror eax,16" "mov al,fs:[si+1180]" "mov ah,fs:[si+1298]" \
	"rol eax,16" "mov es:[di+8],eax" \
	"mov al,fs:[si+1416]" "mov ah,fs:[si+1534]" \
	"ror eax,16" "mov al,fs:[si+1652]" "mov ah,fs:[si+1770]" \
	"rol eax,16" "mov es:[di+12],eax" \
	"mov al,fs:[si+1888]" "mov ah,fs:[si+2006]" \
	"ror eax,16" "mov al,fs:[si+2124]" "mov ah,fs:[si+2242]" \
	"rol eax,16" "mov es:[di+16],eax" \
	"mov al,fs:[si+2361]" "mov ah,fs:[si+2480]" \
	"ror eax,16" "mov al,fs:[si+2599]" "mov ah,fs:[si+2718]" \
	"rol eax,16" "mov es:[di+20],eax" \
	"mov al,fs:[si+2837]" "mov es:[di+24],al" \
	"inc si" \
	"add di,25" \
	"dec cx" \
	"jnz v40data_row" \
	parm [fs si] [es di] modify [ax cx si di];

/* The 25 working ECC registers use 32-byte rows so the four-byte RS kernel
 * has two explicit zero tail bytes. Transpose their first 30 columns into
 * canonical V40-L interleaved order. */
static void dosferInterleaveEccV40(const uint8_t __near *ecc,
		uint8_t __far *result);
#pragma aux dosferInterleaveEccV40 = \
	"mov cx,30" \
	"v40ecc_row:" \
	"mov al,[si]"     "mov es:[di],al" \
	"mov al,[si+32]"  "mov es:[di+1],al" \
	"mov al,[si+64]"  "mov es:[di+2],al" \
	"mov al,[si+96]"  "mov es:[di+3],al" \
	"mov al,[si+128]" "mov es:[di+4],al" \
	"mov al,[si+160]" "mov es:[di+5],al" \
	"mov al,[si+192]" "mov es:[di+6],al" \
	"mov al,[si+224]" "mov es:[di+7],al" \
	"mov al,[si+256]" "mov es:[di+8],al" \
	"mov al,[si+288]" "mov es:[di+9],al" \
	"mov al,[si+320]" "mov es:[di+10],al" \
	"mov al,[si+352]" "mov es:[di+11],al" \
	"mov al,[si+384]" "mov es:[di+12],al" \
	"mov al,[si+416]" "mov es:[di+13],al" \
	"mov al,[si+448]" "mov es:[di+14],al" \
	"mov al,[si+480]" "mov es:[di+15],al" \
	"mov al,[si+512]" "mov es:[di+16],al" \
	"mov al,[si+544]" "mov es:[di+17],al" \
	"mov al,[si+576]" "mov es:[di+18],al" \
	"mov al,[si+608]" "mov es:[di+19],al" \
	"mov al,[si+640]" "mov es:[di+20],al" \
	"mov al,[si+672]" "mov es:[di+21],al" \
	"mov al,[si+704]" "mov es:[di+22],al" \
	"mov al,[si+736]" "mov es:[di+23],al" \
	"mov al,[si+768]" "mov es:[di+24],al" \
	"inc si" \
	"add di,25" \
	"dec cx" \
	"jnz v40ecc_row" \
	parm [si] [es di] modify [ax cx si di];

/* Transpose caller-visible block-major ECC rows (25 * 30 bytes) into the
 * canonical interleaved stream. This is used by the fused RGB encoder, whose
 * public ECC oracle intentionally emits the same block-major layout as the
 * single-channel routine. */
static void dosferInterleaveEcc30V40(const uint8_t __far *ecc,
		uint8_t __far *result);
#pragma aux dosferInterleaveEcc30V40 = \
	"mov cx,30" \
	"v40ecc30_row:" \
	"mov al,fs:[si]"     "mov ah,fs:[si+30]" \
	"ror eax,16" "mov al,fs:[si+60]" "mov ah,fs:[si+90]" \
	"rol eax,16" "mov es:[di],eax" \
	"mov al,fs:[si+120]" "mov ah,fs:[si+150]" \
	"ror eax,16" "mov al,fs:[si+180]" "mov ah,fs:[si+210]" \
	"rol eax,16" "mov es:[di+4],eax" \
	"mov al,fs:[si+240]" "mov ah,fs:[si+270]" \
	"ror eax,16" "mov al,fs:[si+300]" "mov ah,fs:[si+330]" \
	"rol eax,16" "mov es:[di+8],eax" \
	"mov al,fs:[si+360]" "mov ah,fs:[si+390]" \
	"ror eax,16" "mov al,fs:[si+420]" "mov ah,fs:[si+450]" \
	"rol eax,16" "mov es:[di+12],eax" \
	"mov al,fs:[si+480]" "mov ah,fs:[si+510]" \
	"ror eax,16" "mov al,fs:[si+540]" "mov ah,fs:[si+570]" \
	"rol eax,16" "mov es:[di+16],eax" \
	"mov al,fs:[si+600]" "mov ah,fs:[si+630]" \
	"ror eax,16" "mov al,fs:[si+660]" "mov ah,fs:[si+690]" \
	"rol eax,16" "mov es:[di+20],eax" \
	"mov al,fs:[si+720]" "mov es:[di+24],al" \
	"inc si" \
	"add di,25" \
	"dec cx" \
	"jnz v40ecc30_row" \
	parm [fs si] [es di] modify [ax cx si di];

static void dosferRs28Asm(const uint8_t __far *data,uint16_t len,
		const uint8_t __near *table,uint8_t __near *ecc);
#pragma aux dosferRs28Asm = \
	"push bp" \
	"test cx,cx" \
	"jz short rs_done" \
	"rs_loop:" \
	"mov al,es:[si]" \
	"inc si" \
	"xor al,[di]" \
	"xor ah,ah" \
	"shl ax,5" \
	"add ax,bx" \
	"movzx ebp,ax" \
	"mov eax,dword ptr [di+1]" \
	"xor eax,dword ptr ds:[bp]" \
	"mov dword ptr [di],eax" \
	"mov eax,dword ptr [di+5]" \
	"xor eax,dword ptr ds:[bp+4]" \
	"mov dword ptr [di+4],eax" \
	"mov eax,dword ptr [di+9]" \
	"xor eax,dword ptr ds:[bp+8]" \
	"mov dword ptr [di+8],eax" \
	"mov eax,dword ptr [di+13]" \
	"xor eax,dword ptr ds:[bp+12]" \
	"mov dword ptr [di+12],eax" \
	"mov eax,dword ptr [di+17]" \
	"xor eax,dword ptr ds:[bp+16]" \
	"mov dword ptr [di+16],eax" \
	"mov eax,dword ptr [di+21]" \
	"xor eax,dword ptr ds:[bp+20]" \
	"mov dword ptr [di+20],eax" \
	"mov eax,dword ptr [di+25]" \
	"xor eax,dword ptr ds:[bp+24]" \
	"mov dword ptr [di+24],eax" \
	"dec cx" \
	"jnz rs_loop" \
	"rs_done:" \
	"pop bp" \
	parm [es si] [cx] [bx] [di] modify [ax cx dx si];
static void dosferRs26Asm(const uint8_t __far *data,uint16_t len,
		const uint8_t __near *table,uint8_t __near *ecc);
#pragma aux dosferRs26Asm = \
	"push bp" \
	"test cx,cx" \
	"jz short rs26_done" \
	"rs26_loop:" \
	"mov al,es:[si]" \
	"inc si" \
	"xor al,[di]" \
	"xor ah,ah" \
	"shl ax,5" \
	"add ax,bx" \
	"mov bp,ax" \
	"mov eax,dword ptr [di+1]" \
	"xor eax,dword ptr ds:[bp]" \
	"mov dword ptr [di],eax" \
	"mov eax,dword ptr [di+5]" \
	"xor eax,dword ptr ds:[bp+4]" \
	"mov dword ptr [di+4],eax" \
	"mov eax,dword ptr [di+9]" \
	"xor eax,dword ptr ds:[bp+8]" \
	"mov dword ptr [di+8],eax" \
	"mov eax,dword ptr [di+13]" \
	"xor eax,dword ptr ds:[bp+12]" \
	"mov dword ptr [di+12],eax" \
	"mov eax,dword ptr [di+17]" \
	"xor eax,dword ptr ds:[bp+16]" \
	"mov dword ptr [di+16],eax" \
	"mov eax,dword ptr [di+21]" \
	"xor eax,dword ptr ds:[bp+20]" \
	"mov dword ptr [di+20],eax" \
	"mov ax,word ptr [di+25]" \
	"xor ax,word ptr ds:[bp+24]" \
	"mov word ptr [di+24],ax" \
	"dec cx" \
	"jnz rs26_loop" \
	"rs26_done:" \
	"pop bp" \
	parm [es si] [cx] [bx] [di] modify [ax cx dx si];
static void dosferRs30Asm(const uint8_t __far *data,uint16_t len,
		const uint8_t __near *table,uint8_t __near *ecc);
#pragma aux dosferRs30Asm = \
	"push bp" \
	"test cx,cx" \
	"jz short rs30_done" \
	"rs30_loop:" \
	"mov al,es:[si]" \
	"inc si" \
	"xor al,[di]" \
	"xor ah,ah" \
	"shl ax,5" \
	"add ax,bx" \
	"mov bp,ax" \
	"mov eax,dword ptr [di+1]" \
	"xor eax,dword ptr ds:[bp]" \
	"mov dword ptr [di],eax" \
	"mov eax,dword ptr [di+5]" \
	"xor eax,dword ptr ds:[bp+4]" \
	"mov dword ptr [di+4],eax" \
	"mov eax,dword ptr [di+9]" \
	"xor eax,dword ptr ds:[bp+8]" \
	"mov dword ptr [di+8],eax" \
	"mov eax,dword ptr [di+13]" \
	"xor eax,dword ptr ds:[bp+12]" \
	"mov dword ptr [di+12],eax" \
	"mov eax,dword ptr [di+17]" \
	"xor eax,dword ptr ds:[bp+16]" \
	"mov dword ptr [di+16],eax" \
	"mov eax,dword ptr [di+21]" \
	"xor eax,dword ptr ds:[bp+20]" \
	"mov dword ptr [di+20],eax" \
	"mov eax,dword ptr [di+25]" \
	"xor eax,dword ptr ds:[bp+24]" \
	"mov dword ptr [di+24],eax" \
	"mov ax,word ptr [di+29]" \
	"xor ax,word ptr ds:[bp+28]" \
	"mov word ptr [di+28],ax" \
	"dec cx" \
	"jnz rs30_loop" \
	"rs30_done:" \
	"pop bp" \
	parm [es si] [cx] [bx] [di] modify [ax cx dx si];

/* Advance the 30-byte RS register by two input bytes at once:
 * B[j] = E[j+2] ^ row(f1)[j+1] ^ row(f2)[j]. */
static void dosferRs30PairAsm(const uint8_t __far *data,uint16_t len,
		const uint8_t __near *table,uint8_t __near *ecc);
#pragma aux dosferRs30PairAsm = \
	"push bp" \
	"push cx" \
	"shr cx,1" \
	"jz rs30p_pairs_done" \
	"rs30p_loop:" \
	"mov al,es:[si]" \
	"xor al,[di]" \
	"movzx ebp,al" \
	"shl ebp,5" \
	"add bp,bx" \
	"mov al,es:[si+1]" \
	"xor al,[di+1]" \
	"xor al,byte ptr ds:[ebp]" \
	"movzx edx,al" \
	"shl edx,5" \
	"add dx,bx" \
	"add si,2" \
	"mov eax,dword ptr [di+2]" \
	"xor eax,dword ptr ds:[ebp+1]" \
	"xor eax,dword ptr ds:[edx]" \
	"mov dword ptr [di],eax" \
	"mov eax,dword ptr [di+6]" \
	"xor eax,dword ptr ds:[ebp+5]" \
	"xor eax,dword ptr ds:[edx+4]" \
	"mov dword ptr [di+4],eax" \
	"mov eax,dword ptr [di+10]" \
	"xor eax,dword ptr ds:[ebp+9]" \
	"xor eax,dword ptr ds:[edx+8]" \
	"mov dword ptr [di+8],eax" \
	"mov eax,dword ptr [di+14]" \
	"xor eax,dword ptr ds:[ebp+13]" \
	"xor eax,dword ptr ds:[edx+12]" \
	"mov dword ptr [di+12],eax" \
	"mov eax,dword ptr [di+18]" \
	"xor eax,dword ptr ds:[ebp+17]" \
	"xor eax,dword ptr ds:[edx+16]" \
	"mov dword ptr [di+16],eax" \
	"mov eax,dword ptr [di+22]" \
	"xor eax,dword ptr ds:[ebp+21]" \
	"xor eax,dword ptr ds:[edx+20]" \
	"mov dword ptr [di+20],eax" \
	"mov eax,dword ptr [di+26]" \
	"xor eax,dword ptr ds:[ebp+25]" \
	"xor eax,dword ptr ds:[edx+24]" \
	"mov dword ptr [di+24],eax" \
	"mov ax,word ptr ds:[edx+28]" \
	"xor al,byte ptr ds:[ebp+29]" \
	"mov word ptr [di+28],ax" \
	"dec cx" \
	"jnz rs30p_loop" \
	"rs30p_pairs_done:" \
	"pop cx" \
	"test cl,1" \
	"jz rs30p_done" \
	"mov al,es:[si]" \
	"xor al,[di]" \
	"movzx ebp,al" \
	"shl ebp,5" \
	"add bp,bx" \
	"mov eax,dword ptr [di+1]" \
	"xor eax,dword ptr ds:[bp]" \
	"mov dword ptr [di],eax" \
	"mov eax,dword ptr [di+5]" \
	"xor eax,dword ptr ds:[bp+4]" \
	"mov dword ptr [di+4],eax" \
	"mov eax,dword ptr [di+9]" \
	"xor eax,dword ptr ds:[bp+8]" \
	"mov dword ptr [di+8],eax" \
	"mov eax,dword ptr [di+13]" \
	"xor eax,dword ptr ds:[bp+12]" \
	"mov dword ptr [di+12],eax" \
	"mov eax,dword ptr [di+17]" \
	"xor eax,dword ptr ds:[bp+16]" \
	"mov dword ptr [di+16],eax" \
	"mov eax,dword ptr [di+21]" \
	"xor eax,dword ptr ds:[bp+20]" \
	"mov dword ptr [di+20],eax" \
	"mov eax,dword ptr [di+25]" \
	"xor eax,dword ptr ds:[bp+24]" \
	"mov dword ptr [di+24],eax" \
	"mov ax,word ptr [di+29]" \
	"xor ax,word ptr ds:[bp+28]" \
	"mov word ptr [di+28],ax" \
	"rs30p_done:" \
	"pop bp" \
	parm [es si] [cx] [bx] [di] modify [ax cx dx si];

/* Advance the degree-30 register by four bytes while reading and writing the
 * ECC state only once. The caller passes a multiple-of-four length; V40-L's
 * final two or three bytes continue through the proven pair kernel above.
 *
 * D[j] = E[j+4] ^ row(f1)[j+3] ^ row(f2)[j+2]
 *                  ^ row(f3)[j+1] ^ row(f4)[j]. */
static void dosferRs30QuadAsm(const uint8_t __far *data,uint16_t len,
		const uint8_t __near *table,uint8_t __near *ecc);
#pragma aux dosferRs30QuadAsm = \
	"push bp" \
	"push bx" \
	"shr cx,2" \
	"jz rs30q_done" \
	"push cx" \
	"rs30q_loop:" \
	"mov al,es:[si]" \
	"xor al,[di]" \
	"xor ah,ah" \
	"shl ax,5" \
	"add ax,bx" \
	"movzx ebp,ax" \
	"mov al,es:[si+1]" \
	"xor al,[di+1]" \
	"xor al,byte ptr ds:[ebp]" \
	"xor ah,ah" \
	"shl ax,5" \
	"add ax,bx" \
	"movzx edx,ax" \
	"mov al,es:[si+2]" \
	"xor al,[di+2]" \
	"xor al,byte ptr ds:[ebp+1]" \
	"xor al,byte ptr ds:[edx]" \
	"xor ah,ah" \
	"shl ax,5" \
	"add ax,bx" \
	"movzx ecx,ax" \
	"mov al,es:[si+3]" \
	"xor al,[di+3]" \
	"xor al,byte ptr ds:[ebp+2]" \
	"xor al,byte ptr ds:[edx+1]" \
	"xor al,byte ptr ds:[ecx]" \
	"xor ah,ah" \
	"shl ax,5" \
	"add ax,bx" \
	"movzx ebx,ax" \
	"mov eax,dword ptr [di+4]" \
	"xor eax,dword ptr ds:[ebp+3]" \
	"xor eax,dword ptr ds:[edx+2]" \
	"xor eax,dword ptr ds:[ecx+1]" \
	"xor eax,dword ptr ds:[ebx]" \
	"mov dword ptr [di],eax" \
	"mov eax,dword ptr [di+8]" \
	"xor eax,dword ptr ds:[ebp+7]" \
	"xor eax,dword ptr ds:[edx+6]" \
	"xor eax,dword ptr ds:[ecx+5]" \
	"xor eax,dword ptr ds:[ebx+4]" \
	"mov dword ptr [di+4],eax" \
	"mov eax,dword ptr [di+12]" \
	"xor eax,dword ptr ds:[ebp+11]" \
	"xor eax,dword ptr ds:[edx+10]" \
	"xor eax,dword ptr ds:[ecx+9]" \
	"xor eax,dword ptr ds:[ebx+8]" \
	"mov dword ptr [di+8],eax" \
	"mov eax,dword ptr [di+16]" \
	"xor eax,dword ptr ds:[ebp+15]" \
	"xor eax,dword ptr ds:[edx+14]" \
	"xor eax,dword ptr ds:[ecx+13]" \
	"xor eax,dword ptr ds:[ebx+12]" \
	"mov dword ptr [di+12],eax" \
	"mov eax,dword ptr [di+20]" \
	"xor eax,dword ptr ds:[ebp+19]" \
	"xor eax,dword ptr ds:[edx+18]" \
	"xor eax,dword ptr ds:[ecx+17]" \
	"xor eax,dword ptr ds:[ebx+16]" \
	"mov dword ptr [di+16],eax" \
	"mov eax,dword ptr [di+24]" \
	"xor eax,dword ptr ds:[ebp+23]" \
	"xor eax,dword ptr ds:[edx+22]" \
	"xor eax,dword ptr ds:[ecx+21]" \
	"xor eax,dword ptr ds:[ebx+20]" \
	"mov dword ptr [di+20],eax" \
	"mov eax,dword ptr [di+28]" \
	"xor eax,dword ptr ds:[ebp+27]" \
	"xor eax,dword ptr ds:[edx+26]" \
	"xor eax,dword ptr ds:[ecx+25]" \
	"xor eax,dword ptr ds:[ebx+24]" \
	"mov dword ptr [di+24],eax" \
	"mov ax,word ptr ds:[ebx+28]" \
	"xor al,byte ptr ds:[ecx+29]" \
	"mov word ptr [di+28],ax" \
	"mov bx,word ptr ss:[esp+2]" \
	"add si,4" \
	"dec word ptr ss:[esp]" \
	"jnz rs30q_loop" \
	"add sp,2" \
	"rs30q_done:" \
	"pop bx" \
	"pop bp" \
	parm [es si] [cx] [bx] [di] modify [ax bx cx dx si];

/* Fused three-channel version of the proven four-byte transition. R and G
 * share one checked far offset in SI while ES/FS/GS select the channel. Each
 * channel has an independent 32-byte near ECC state; only source traversal
 * and quad loop control are shared. */
static void dosferRs30Quad3Asm(const uint8_t __far *red,
		const uint8_t __far *green,uint16_t len);
#pragma aux dosferRs30Quad3Asm = \
	"push gs" \
	"mov ax,word ptr dosferRsBlue3+2" \
	"mov gs,ax" \
	"shr cx,2" \
	"jz rs30q3_done" \
	"push cx" \
	"rs30q3_loop:" \
	/* Red factors and state transition. */ \
	"mov al,es:[si]" "xor al,byte ptr dosferRsEcc3" \
	"movzx edi,al" "shl edi,5" \
	"mov al,es:[si+1]" "xor al,byte ptr dosferRsEcc3+1" \
	"xor al,byte ptr ds:dosferRsStep[edi]" \
	"movzx edx,al" "shl edx,5" \
	"mov al,es:[si+2]" "xor al,byte ptr dosferRsEcc3+2" \
	"xor al,byte ptr ds:dosferRsStep[edi+1]" \
	"xor al,byte ptr ds:dosferRsStep[edx]" \
	"movzx ecx,al" "shl ecx,5" \
	"mov al,es:[si+3]" "xor al,byte ptr dosferRsEcc3+3" \
	"xor al,byte ptr ds:dosferRsStep[edi+2]" \
	"xor al,byte ptr ds:dosferRsStep[edx+1]" \
	"xor al,byte ptr ds:dosferRsStep[ecx]" \
	"movzx ebx,al" "shl ebx,5" \
	"mov eax,dword ptr dosferRsEcc3+4" \
	"xor eax,dword ptr ds:dosferRsStep[edi+3]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+2]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+1]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx]" \
	"mov dword ptr dosferRsEcc3,eax" \
	"mov eax,dword ptr dosferRsEcc3+8" \
	"xor eax,dword ptr ds:dosferRsStep[edi+7]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+6]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+5]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+4]" \
	"mov dword ptr dosferRsEcc3+4,eax" \
	"mov eax,dword ptr dosferRsEcc3+12" \
	"xor eax,dword ptr ds:dosferRsStep[edi+11]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+10]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+9]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+8]" \
	"mov dword ptr dosferRsEcc3+8,eax" \
	"mov eax,dword ptr dosferRsEcc3+16" \
	"xor eax,dword ptr ds:dosferRsStep[edi+15]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+14]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+13]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+12]" \
	"mov dword ptr dosferRsEcc3+12,eax" \
	"mov eax,dword ptr dosferRsEcc3+20" \
	"xor eax,dword ptr ds:dosferRsStep[edi+19]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+18]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+17]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+16]" \
	"mov dword ptr dosferRsEcc3+16,eax" \
	"mov eax,dword ptr dosferRsEcc3+24" \
	"xor eax,dword ptr ds:dosferRsStep[edi+23]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+22]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+21]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+20]" \
	"mov dword ptr dosferRsEcc3+20,eax" \
	"mov eax,dword ptr dosferRsEcc3+28" \
	"xor eax,dword ptr ds:dosferRsStep[edi+27]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+26]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+25]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+24]" \
	"mov dword ptr dosferRsEcc3+24,eax" \
	"mov ax,word ptr ds:dosferRsStep[ebx+28]" \
	"xor al,byte ptr ds:dosferRsStep[ecx+29]" \
	"mov word ptr dosferRsEcc3+28,ax" \
	/* Green factors and state transition. */ \
	"mov al,fs:[si]" "xor al,byte ptr dosferRsEcc3+32" \
	"movzx edi,al" "shl edi,5" \
	"mov al,fs:[si+1]" "xor al,byte ptr dosferRsEcc3+33" \
	"xor al,byte ptr ds:dosferRsStep[edi]" \
	"movzx edx,al" "shl edx,5" \
	"mov al,fs:[si+2]" "xor al,byte ptr dosferRsEcc3+34" \
	"xor al,byte ptr ds:dosferRsStep[edi+1]" \
	"xor al,byte ptr ds:dosferRsStep[edx]" \
	"movzx ecx,al" "shl ecx,5" \
	"mov al,fs:[si+3]" "xor al,byte ptr dosferRsEcc3+35" \
	"xor al,byte ptr ds:dosferRsStep[edi+2]" \
	"xor al,byte ptr ds:dosferRsStep[edx+1]" \
	"xor al,byte ptr ds:dosferRsStep[ecx]" \
	"movzx ebx,al" "shl ebx,5" \
	"mov eax,dword ptr dosferRsEcc3+36" \
	"xor eax,dword ptr ds:dosferRsStep[edi+3]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+2]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+1]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx]" \
	"mov dword ptr dosferRsEcc3+32,eax" \
	"mov eax,dword ptr dosferRsEcc3+40" \
	"xor eax,dword ptr ds:dosferRsStep[edi+7]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+6]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+5]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+4]" \
	"mov dword ptr dosferRsEcc3+36,eax" \
	"mov eax,dword ptr dosferRsEcc3+44" \
	"xor eax,dword ptr ds:dosferRsStep[edi+11]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+10]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+9]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+8]" \
	"mov dword ptr dosferRsEcc3+40,eax" \
	"mov eax,dword ptr dosferRsEcc3+48" \
	"xor eax,dword ptr ds:dosferRsStep[edi+15]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+14]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+13]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+12]" \
	"mov dword ptr dosferRsEcc3+44,eax" \
	"mov eax,dword ptr dosferRsEcc3+52" \
	"xor eax,dword ptr ds:dosferRsStep[edi+19]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+18]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+17]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+16]" \
	"mov dword ptr dosferRsEcc3+48,eax" \
	"mov eax,dword ptr dosferRsEcc3+56" \
	"xor eax,dword ptr ds:dosferRsStep[edi+23]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+22]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+21]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+20]" \
	"mov dword ptr dosferRsEcc3+52,eax" \
	"mov eax,dword ptr dosferRsEcc3+60" \
	"xor eax,dword ptr ds:dosferRsStep[edi+27]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+26]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+25]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+24]" \
	"mov dword ptr dosferRsEcc3+56,eax" \
	"mov ax,word ptr ds:dosferRsStep[ebx+28]" \
	"xor al,byte ptr ds:dosferRsStep[ecx+29]" \
	"mov word ptr dosferRsEcc3+60,ax" \
	/* Blue factors and state transition. */ \
	"mov al,gs:[si]" "xor al,byte ptr dosferRsEcc3+64" \
	"movzx edi,al" "shl edi,5" \
	"mov al,gs:[si+1]" "xor al,byte ptr dosferRsEcc3+65" \
	"xor al,byte ptr ds:dosferRsStep[edi]" \
	"movzx edx,al" "shl edx,5" \
	"mov al,gs:[si+2]" "xor al,byte ptr dosferRsEcc3+66" \
	"xor al,byte ptr ds:dosferRsStep[edi+1]" \
	"xor al,byte ptr ds:dosferRsStep[edx]" \
	"movzx ecx,al" "shl ecx,5" \
	"mov al,gs:[si+3]" "xor al,byte ptr dosferRsEcc3+67" \
	"xor al,byte ptr ds:dosferRsStep[edi+2]" \
	"xor al,byte ptr ds:dosferRsStep[edx+1]" \
	"xor al,byte ptr ds:dosferRsStep[ecx]" \
	"movzx ebx,al" "shl ebx,5" \
	"mov eax,dword ptr dosferRsEcc3+68" \
	"xor eax,dword ptr ds:dosferRsStep[edi+3]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+2]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+1]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx]" \
	"mov dword ptr dosferRsEcc3+64,eax" \
	"mov eax,dword ptr dosferRsEcc3+72" \
	"xor eax,dword ptr ds:dosferRsStep[edi+7]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+6]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+5]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+4]" \
	"mov dword ptr dosferRsEcc3+68,eax" \
	"mov eax,dword ptr dosferRsEcc3+76" \
	"xor eax,dword ptr ds:dosferRsStep[edi+11]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+10]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+9]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+8]" \
	"mov dword ptr dosferRsEcc3+72,eax" \
	"mov eax,dword ptr dosferRsEcc3+80" \
	"xor eax,dword ptr ds:dosferRsStep[edi+15]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+14]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+13]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+12]" \
	"mov dword ptr dosferRsEcc3+76,eax" \
	"mov eax,dword ptr dosferRsEcc3+84" \
	"xor eax,dword ptr ds:dosferRsStep[edi+19]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+18]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+17]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+16]" \
	"mov dword ptr dosferRsEcc3+80,eax" \
	"mov eax,dword ptr dosferRsEcc3+88" \
	"xor eax,dword ptr ds:dosferRsStep[edi+23]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+22]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+21]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+20]" \
	"mov dword ptr dosferRsEcc3+84,eax" \
	"mov eax,dword ptr dosferRsEcc3+92" \
	"xor eax,dword ptr ds:dosferRsStep[edi+27]" \
	"xor eax,dword ptr ds:dosferRsStep[edx+26]" \
	"xor eax,dword ptr ds:dosferRsStep[ecx+25]" \
	"xor eax,dword ptr ds:dosferRsStep[ebx+24]" \
	"mov dword ptr dosferRsEcc3+88,eax" \
	"mov ax,word ptr ds:dosferRsStep[ebx+28]" \
	"xor al,byte ptr ds:dosferRsStep[ecx+29]" \
	"mov word ptr dosferRsEcc3+92,ax" \
	"add si,4" \
	"dec word ptr ss:[esp]" \
	"jnz rs30q3_loop" \
	"add sp,2" \
	"rs30q3_done:" \
	"pop gs" \
	parm [es si] [fs di] [cx] modify [ax bx cx dx si di];

/* Export the three independent 30-byte ECC states with one segment setup.
 * The fused encoder aligns all three workspaces to the same far offset; the
 * caller retains generic memcpy fallback for any unaligned external use. */
static void dosferCopyEcc3Asm(uint8_t __far *red,uint8_t __far *green);
#pragma aux dosferCopyEcc3Asm = \
	"push bx" "push ds" "push gs" \
	"push ss" "pop ds" \
	"mov bx,word ptr dosferRsEccBlueOut3" \
	"mov ax,word ptr dosferRsEccBlueOut3+2" "mov gs,ax" \
	"mov eax,dword ptr dosferRsEcc3" "mov es:[di],eax" \
	"mov eax,dword ptr dosferRsEcc3+4" "mov es:[di+4],eax" \
	"mov eax,dword ptr dosferRsEcc3+8" "mov es:[di+8],eax" \
	"mov eax,dword ptr dosferRsEcc3+12" "mov es:[di+12],eax" \
	"mov eax,dword ptr dosferRsEcc3+16" "mov es:[di+16],eax" \
	"mov eax,dword ptr dosferRsEcc3+20" "mov es:[di+20],eax" \
	"mov eax,dword ptr dosferRsEcc3+24" "mov es:[di+24],eax" \
	"mov ax,word ptr dosferRsEcc3+28" "mov es:[di+28],ax" \
	"mov eax,dword ptr dosferRsEcc3+32" "mov fs:[si],eax" \
	"mov eax,dword ptr dosferRsEcc3+36" "mov fs:[si+4],eax" \
	"mov eax,dword ptr dosferRsEcc3+40" "mov fs:[si+8],eax" \
	"mov eax,dword ptr dosferRsEcc3+44" "mov fs:[si+12],eax" \
	"mov eax,dword ptr dosferRsEcc3+48" "mov fs:[si+16],eax" \
	"mov eax,dword ptr dosferRsEcc3+52" "mov fs:[si+20],eax" \
	"mov eax,dword ptr dosferRsEcc3+56" "mov fs:[si+24],eax" \
	"mov ax,word ptr dosferRsEcc3+60" "mov fs:[si+28],ax" \
	"mov eax,dword ptr dosferRsEcc3+64" "mov gs:[bx],eax" \
	"mov eax,dword ptr dosferRsEcc3+68" "mov gs:[bx+4],eax" \
	"mov eax,dword ptr dosferRsEcc3+72" "mov gs:[bx+8],eax" \
	"mov eax,dword ptr dosferRsEcc3+76" "mov gs:[bx+12],eax" \
	"mov eax,dword ptr dosferRsEcc3+80" "mov gs:[bx+16],eax" \
	"mov eax,dword ptr dosferRsEcc3+84" "mov gs:[bx+20],eax" \
	"mov eax,dword ptr dosferRsEcc3+88" "mov gs:[bx+24],eax" \
	"mov ax,word ptr dosferRsEcc3+92" "mov gs:[bx+28],ax" \
	"pop gs" "pop ds" "pop bx" \
	parm [es di] [fs si] modify [ax];

#endif

/* DOSfer 386 fast path: 768 bytes avoid eight shift/XOR rounds for every
 * GF(256) multiplication. DOS is single-threaded, so lazy initialization is
 * safe and Nayuki's public API remains unchanged. */
static uint8_t dosferGfExp[512];
static uint8_t dosferGfLog[256];
static bool dosferGfReady = false;
static bool dosferCodewordsOnly = false;
static bool dosferAlignedFast = true;
static uint8_t dosferRsDiv[qrcodegen_REED_SOLOMON_DEGREE_MAX];
static uint8_t dosferRsDivLog[qrcodegen_REED_SOLOMON_DEGREE_MAX];
static int dosferRsDegree;
void qrcodegen_dosferSetCodewordsOnly(bool enabled) { dosferCodewordsOnly = enabled; }
void qrcodegen_dosferSetAlignedFast(bool enabled) { dosferAlignedFast = enabled; }
int qrcodegen_dosferCodewordBytes(int version) {
	return version >= 1 && version <= 40 ? getNumRawDataModules(version) / 8 : 0;
}
int qrcodegen_dosferDataCodewordBytes(int version, enum qrcodegen_Ecc ecl) {
	if(version < 1 || version > 40 || (int)ecl < 0 || (int)ecl > 3)return 0;
	return getNumDataCodewords(version,ecl);
}
static void dosferInitGf(void) {
	unsigned int x = 1;
	for (int i = 0; i < 255; i++) {
		dosferGfExp[i] = (uint8_t)x;
		dosferGfLog[x] = (uint8_t)i;
		x <<= 1;
		if (x & 0x100)
			x ^= 0x11D;
	}
	for (int i = 255; i < 512; i++)
		dosferGfExp[i] = dosferGfExp[i - 255];
	dosferGfReady = true;
}
static void dosferPrepareRs(int degree) {
	if(dosferRsDegree==degree)return;
	reedSolomonComputeDivisor(degree,dosferRsDiv);
	if(!dosferGfReady)dosferInitGf();
	for(int j=0;j<degree;j++)dosferRsDivLog[j]=dosferRsDiv[j]?dosferGfLog[dosferRsDiv[j]]:0;
	for(int factor=0;factor<256;factor++){
		uint8_t *row=dosferRsStep+(unsigned)factor*DOSFER_RS_STRIDE;
		if(factor==0)memset(row,0,DOSFER_RS_STRIDE);
		else{int factorLog=dosferGfLog[factor];for(int j=0;j<degree;j++)row[j]=dosferGfExp[dosferRsDivLog[j]+factorLog];}
	}
	dosferRsDegree=degree;
}

// For generating error correction codes.
testable const int8_t NUM_ERROR_CORRECTION_BLOCKS[4][41] = {
	// Version: (note that index 0 is for padding, and is set to an illegal value)
	//0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40    Error correction level
	{-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4,  4,  4,  4,  4,  6,  6,  6,  6,  7,  8,  8,  9,  9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},  // Low
	{-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5,  5,  8,  9,  9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},  // Medium
	{-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8,  8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},  // Quartile
	{-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},  // High
};

// For automatic mask pattern selection.
static const int PENALTY_N1 =  3;
static const int PENALTY_N2 =  3;
static const int PENALTY_N3 = 40;
static const int PENALTY_N4 = 10;



/*---- High-level QR Code encoding functions ----*/

// Public function - see documentation comment in header file.
bool qrcodegen_encodeText(const char *text, uint8_t tempBuffer[], uint8_t qrcode[],
		enum qrcodegen_Ecc ecl, int minVersion, int maxVersion, enum qrcodegen_Mask mask, bool boostEcl) {
	
	size_t textLen = strlen(text);
	if (textLen == 0)
		return qrcodegen_encodeSegmentsAdvanced(NULL, 0, ecl, minVersion, maxVersion, mask, boostEcl, tempBuffer, qrcode);
	size_t bufLen = (size_t)qrcodegen_BUFFER_LEN_FOR_VERSION(maxVersion);
	
	struct qrcodegen_Segment seg;
	if (qrcodegen_isNumeric(text)) {
		if (qrcodegen_calcSegmentBufferSize(qrcodegen_Mode_NUMERIC, textLen) > bufLen)
			goto fail;
		seg = qrcodegen_makeNumeric(text, tempBuffer);
	} else if (qrcodegen_isAlphanumeric(text)) {
		if (qrcodegen_calcSegmentBufferSize(qrcodegen_Mode_ALPHANUMERIC, textLen) > bufLen)
			goto fail;
		seg = qrcodegen_makeAlphanumeric(text, tempBuffer);
	} else {
		if (textLen > bufLen)
			goto fail;
		for (size_t i = 0; i < textLen; i++)
			tempBuffer[i] = (uint8_t)text[i];
		seg.mode = qrcodegen_Mode_BYTE;
		seg.bitLength = calcSegmentBitLength(seg.mode, textLen);
		if (seg.bitLength == LENGTH_OVERFLOW)
			goto fail;
		seg.numChars = (int)textLen;
		seg.data = tempBuffer;
	}
	return qrcodegen_encodeSegmentsAdvanced(&seg, 1, ecl, minVersion, maxVersion, mask, boostEcl, tempBuffer, qrcode);
	
fail:
	qrcode[0] = 0;  // Set size to invalid value for safety
	return false;
}


// Public function - see documentation comment in header file.
bool qrcodegen_encodeBinary(uint8_t dataAndTemp[], size_t dataLen, uint8_t qrcode[],
		enum qrcodegen_Ecc ecl, int minVersion, int maxVersion, enum qrcodegen_Mask mask, bool boostEcl) {
	
	struct qrcodegen_Segment seg;
	seg.mode = qrcodegen_Mode_BYTE;
	seg.bitLength = calcSegmentBitLength(seg.mode, dataLen);
	if (seg.bitLength == LENGTH_OVERFLOW) {
		qrcode[0] = 0;  // Set size to invalid value for safety
		return false;
	}
	seg.numChars = (int)dataLen;
	seg.data = dataAndTemp;
	return qrcodegen_encodeSegmentsAdvanced(&seg, 1, ecl, minVersion, maxVersion, mask, boostEcl, dataAndTemp, qrcode);
}

bool qrcodegen_encodeBinaryAligned(uint8_t dataAndTemp[], size_t dataLen, uint8_t qrcode[],
		enum qrcodegen_Ecc ecl, int minVersion, int maxVersion, enum qrcodegen_Mask mask, bool boostEcl) {
	uint8_t eciData[1];
	struct qrcodegen_Segment segs[2];
	segs[0] = qrcodegen_makeEci(3, eciData);
	segs[1].mode = qrcodegen_Mode_BYTE;
	segs[1].bitLength = calcSegmentBitLength(segs[1].mode, dataLen);
	if (segs[1].bitLength == LENGTH_OVERFLOW) { qrcode[0] = 0; return false; }
	segs[1].numChars = (int)dataLen;
	segs[1].data = dataAndTemp;
	return qrcodegen_encodeSegmentsAdvanced(segs, 2, ecl, minVersion, maxVersion,
		mask, boostEcl, dataAndTemp, qrcode);
}


// Appends the given number of low-order bits of the given value to the given byte-based
// bit buffer, increasing the bit length. Requires 0 <= numBits <= 16 and val < 2^numBits.
testable void appendBitsToBuffer(unsigned int val, int numBits, uint8_t buffer[], int *bitLen) {
	assert(0 <= numBits && numBits <= 16 && (unsigned long)val >> numBits == 0);
	for (int i = numBits - 1; i >= 0; i--, (*bitLen)++)
		buffer[*bitLen >> 3] |= ((val >> i) & 1) << (7 - (*bitLen & 7));
}



/*---- Low-level QR Code encoding functions ----*/

// Public function - see documentation comment in header file.
bool qrcodegen_encodeSegments(const struct qrcodegen_Segment segs[], size_t len,
		enum qrcodegen_Ecc ecl, uint8_t tempBuffer[], uint8_t qrcode[]) {
	return qrcodegen_encodeSegmentsAdvanced(segs, len, ecl,
		qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true, tempBuffer, qrcode);
}


// Public function - see documentation comment in header file.
bool qrcodegen_encodeSegmentsAdvanced(const struct qrcodegen_Segment segs[], size_t len, enum qrcodegen_Ecc ecl,
		int minVersion, int maxVersion, enum qrcodegen_Mask mask, bool boostEcl, uint8_t tempBuffer[], uint8_t qrcode[]) {
	assert(segs != NULL || len == 0);
	assert(qrcodegen_VERSION_MIN <= minVersion && minVersion <= maxVersion && maxVersion <= qrcodegen_VERSION_MAX);
	assert(0 <= (int)ecl && (int)ecl <= 3 && -1 <= (int)mask && (int)mask <= 7);
	DOSFER_PROFILE_START;
	
	// Find the minimal version number to use
	int version, dataUsedBits;
	for (version = minVersion; ; version++) {
		int dataCapacityBits = getNumDataCodewords(version, ecl) * 8;  // Number of data bits available
		dataUsedBits = getTotalBits(segs, len, version);
		if (dataUsedBits != LENGTH_OVERFLOW && dataUsedBits <= dataCapacityBits)
			break;  // This version number is found to be suitable
		if (version >= maxVersion) {  // All versions in the range could not fit the given data
			qrcode[0] = 0;  // Set size to invalid value for safety
			return false;
		}
	}
	assert(dataUsedBits != LENGTH_OVERFLOW);
	
	// Increase the error correction level while the data still fits in the current version number
	for (int i = (int)qrcodegen_Ecc_MEDIUM; i <= (int)qrcodegen_Ecc_HIGH; i++) {  // From low to high
		if (boostEcl && dataUsedBits <= getNumDataCodewords(version, (enum qrcodegen_Ecc)i) * 8)
			ecl = (enum qrcodegen_Ecc)i;
	}
	
	// Concatenate all segments to create the data bit string
	memset(qrcode, 0, (size_t)qrcodegen_BUFFER_LEN_FOR_VERSION(version) * sizeof(qrcode[0]));
	int bitLen = 0;
	if (dosferAlignedFast && len == 2 && version >= 10 &&
			segs[0].mode == qrcodegen_Mode_ECI && segs[0].bitLength == 8 &&
			segs[0].data[0] == 3 && segs[1].mode == qrcodegen_Mode_BYTE) {
		const struct qrcodegen_Segment *seg = &segs[1];
		int n = seg->numChars;
		qrcode[0] = 0x70; qrcode[1] = 0x34;
		qrcode[2] = (uint8_t)((unsigned)n >> 8); qrcode[3] = (uint8_t)n;
		if (n > 0) memcpy(qrcode + 4, seg->data, (size_t)n);
		bitLen = 32 + n * 8;
	} else if (len == 1 && segs[0].mode == qrcodegen_Mode_BYTE) {
		/* Fixed-version DOSfer frames are one byte segment. Their 4-bit mode and
		 * 16-bit length leave a constant nibble shift, so pack whole bytes instead
		 * of calling the one-bit appender roughly 3,300 times per frame. */
		const struct qrcodegen_Segment *seg = &segs[0];
		int n = seg->numChars;
		int dataStart;
		if (version < 10) {
			qrcode[0] = (uint8_t)(0x40 | ((unsigned)n >> 4));
			qrcode[1] = (uint8_t)((unsigned)n << 4);
			dataStart = 1; bitLen = 12 + n * 8;
		} else {
			qrcode[0] = (uint8_t)(0x40 | ((unsigned)n >> 12));
			qrcode[1] = (uint8_t)((unsigned)n >> 4);
			qrcode[2] = (uint8_t)((unsigned)n << 4);
			dataStart = 2; bitLen = 20 + n * 8;
		}
		if (n > 0) {
			int i = 0;
			qrcode[dataStart] |= (uint8_t)(seg->data[0] >> 4);
			/* A 386 can merge four adjacent nibble pairs with two unaligned
			 * dword loads and one store. Masking after each shift prevents
			 * carries between the four byte lanes. */
			for (; i + 4 < n; i += 4) {
				uint32_t lo = *(const uint32_t *)(seg->data + i);
				uint32_t hi = *(const uint32_t *)(seg->data + i + 1);
				*(uint32_t *)(qrcode + dataStart + i + 1) =
					((lo << 4) & UINT32_C(0xF0F0F0F0)) |
					((hi >> 4) & UINT32_C(0x0F0F0F0F));
			}
			for (; i + 1 < n; i++)
				qrcode[dataStart + i + 1] = (uint8_t)(seg->data[i] << 4 | seg->data[i + 1] >> 4);
			qrcode[dataStart + n] = (uint8_t)(seg->data[n - 1] << 4);
		}
	} else for (size_t i = 0; i < len; i++) {
		const struct qrcodegen_Segment *seg = &segs[i];
		appendBitsToBuffer((unsigned int)seg->mode, 4, qrcode, &bitLen);
		appendBitsToBuffer((unsigned int)seg->numChars, numCharCountBits(seg->mode, version), qrcode, &bitLen);
		for (int j = 0; j < seg->bitLength; j++) {
			int bit = (seg->data[j >> 3] >> (7 - (j & 7))) & 1;
			appendBitsToBuffer((unsigned int)bit, 1, qrcode, &bitLen);
		}
	}
	assert(bitLen == dataUsedBits);
	
	// Add terminator and pad up to a byte if applicable
	int dataCapacityBits = getNumDataCodewords(version, ecl) * 8;
	assert(bitLen <= dataCapacityBits);
	int terminatorBits = dataCapacityBits - bitLen;
	if (terminatorBits > 4)
		terminatorBits = 4;
	appendBitsToBuffer(0, terminatorBits, qrcode, &bitLen);
	appendBitsToBuffer(0, (8 - bitLen % 8) % 8, qrcode, &bitLen);
	assert(bitLen % 8 == 0);
	
	// Pad with alternating bytes until data capacity is reached
	for (uint8_t padByte = 0xEC; bitLen < dataCapacityBits; padByte ^= 0xEC ^ 0x11)
		appendBitsToBuffer(padByte, 8, qrcode, &bitLen);
	DOSFER_PROFILE_MARK(0);
	
	// Compute ECC, draw modules
	addEccAndInterleave(qrcode, version, ecl, tempBuffer);
	DOSFER_PROFILE_MARK(1);
	if (dosferCodewordsOnly)
		return true;
	bool dosferCached = mask != qrcodegen_Mask_AUTO && dosferPrepareMatrixCache(version, ecl, mask);
	if (dosferCached) {
		memcpy(qrcode, dosferFunctionTemplate,
			(size_t)qrcodegen_BUFFER_LEN_FOR_VERSION(version));
		DOSFER_PROFILE_MARK(2);
		dosferDrawCodewordsCached(tempBuffer, getNumRawDataModules(version) / 8, qrcode);
		DOSFER_PROFILE_MARK(3);
		DOSFER_PROFILE_MARK(4);
	} else {
		initializeFunctionModules(version, qrcode);
		DOSFER_PROFILE_MARK(2);
		drawCodewords(tempBuffer, getNumRawDataModules(version) / 8, qrcode);
		drawLightFunctionModules(qrcode, version);
		DOSFER_PROFILE_MARK(3);
		initializeFunctionModules(version, tempBuffer);
		DOSFER_PROFILE_MARK(4);
	}
	
	// Do masking
	if (mask == qrcodegen_Mask_AUTO) {  // Automatically choose best mask
		long minPenalty = LONG_MAX;
		for (int i = 0; i < 8; i++) {
			enum qrcodegen_Mask msk = (enum qrcodegen_Mask)i;
			applyMask(tempBuffer, qrcode, msk);
			drawFormatBits(ecl, msk, qrcode);
			long penalty = getPenaltyScore(qrcode);
			if (penalty < minPenalty) {
				mask = msk;
				minPenalty = penalty;
			}
			applyMask(tempBuffer, qrcode, msk);  // Undoes the mask due to XOR
		}
	}
	assert(0 <= (int)mask && (int)mask <= 7);
	if (dosferCached) {
		DOSFER_PROFILE_MARK(5);
		return true;
	}
	applyMask(tempBuffer, qrcode, mask);  // Apply the final choice of mask
	drawFormatBits(ecl, mask, qrcode);  // Overwrite old format bits
	DOSFER_PROFILE_MARK(5);
	return true;
}



/*---- Error correction code generation functions ----*/

// Appends error correction bytes to each block of the given data array, then interleaves
// bytes from the blocks and stores them in the result array. data[0 : dataLen] contains
// the input data. data[dataLen : rawCodewords] is used as a temporary work area and will
// be clobbered by this function. The final answer is stored in result[0 : rawCodewords].
testable void addEccAndInterleave(uint8_t data[], int version, enum qrcodegen_Ecc ecl, uint8_t result[]) {
	// Calculate parameter numbers
	assert(0 <= (int)ecl && (int)ecl < 4 && qrcodegen_VERSION_MIN <= version && version <= qrcodegen_VERSION_MAX);
	int numBlocks = NUM_ERROR_CORRECTION_BLOCKS[(int)ecl][version];
	int blockEccLen = ECC_CODEWORDS_PER_BLOCK  [(int)ecl][version];
	int rawCodewords = getNumRawDataModules(version) / 8;
	int dataLen = getNumDataCodewords(version, ecl);
	int numShortBlocks = numBlocks - rawCodewords % numBlocks;
	int shortBlockDataLen = rawCodewords / numBlocks - blockEccLen;
	
	// Split data into blocks, calculate ECC, and interleave
	// (not concatenate) the bytes into a single sequence
	dosferPrepareRs(blockEccLen);
	uint8_t
#ifdef __WATCOMC__
		__near
#endif
		*rsStep=dosferRsStep;
	const uint8_t *dat = data;
	for (int i = 0; i < numBlocks; i++) {
		int datLen = shortBlockDataLen + (i < numShortBlocks ? 0 : 1);
		static uint8_t
#ifdef __WATCOMC__
			__near
#endif
			eccLocal[qrcodegen_REED_SOLOMON_DEGREE_MAX + 1];
		uint8_t *ecc = eccLocal;
		memset(ecc, 0, (size_t)blockEccLen);
		if (blockEccLen == 28) {
			/* V40-M uses 28 ECC bytes per block. Shift by one byte and XOR
			 * two feedback bytes at a time; unrolling removes the hottest
			 * loop/control overhead on a 386. The extra zero byte supplies
			 * the implicit shifted-in zero for the final pair. */
			#ifdef __WATCOMC__
			dosferRs28Asm(dat,(uint16_t)datLen,rsStep,eccLocal);
			#else
			for (int d = 0; d < datLen; d++) {
				uint8_t factor = dat[d] ^ ecc[0];
				const uint8_t *row=rsStep+(unsigned)factor*DOSFER_RS_STRIDE;
				ecc[28]=0;
				#define DOSFER_RS_PAIR(k) (*(uint16_t *)(ecc+(k)*2)=(uint16_t)(*(const uint16_t *)(ecc+(k)*2+1)^*(const uint16_t *)(row+(k)*2)))
				DOSFER_RS_PAIR(0); DOSFER_RS_PAIR(1); DOSFER_RS_PAIR(2); DOSFER_RS_PAIR(3);
				DOSFER_RS_PAIR(4); DOSFER_RS_PAIR(5); DOSFER_RS_PAIR(6); DOSFER_RS_PAIR(7);
				DOSFER_RS_PAIR(8); DOSFER_RS_PAIR(9); DOSFER_RS_PAIR(10);DOSFER_RS_PAIR(11);
				DOSFER_RS_PAIR(12);DOSFER_RS_PAIR(13);
				#undef DOSFER_RS_PAIR
			}
			#endif
		} else if (blockEccLen == 26) {
			#ifdef __WATCOMC__
			eccLocal[26]=0;
			dosferRs26Asm(dat,(uint16_t)datLen,rsStep,eccLocal);
			#else
			for (int d = 0; d < datLen; d++) {
				uint8_t factor = dat[d] ^ ecc[0];
				const uint8_t *row=rsStep+(unsigned)factor*DOSFER_RS_STRIDE;
				ecc[26]=0;
				#define DOSFER_RS26_PAIR(k) (*(uint16_t *)(ecc+(k)*2)=(uint16_t)(*(const uint16_t *)(ecc+(k)*2+1)^*(const uint16_t *)(row+(k)*2)))
				DOSFER_RS26_PAIR(0); DOSFER_RS26_PAIR(1); DOSFER_RS26_PAIR(2); DOSFER_RS26_PAIR(3);
				DOSFER_RS26_PAIR(4); DOSFER_RS26_PAIR(5); DOSFER_RS26_PAIR(6); DOSFER_RS26_PAIR(7);
				DOSFER_RS26_PAIR(8); DOSFER_RS26_PAIR(9); DOSFER_RS26_PAIR(10);DOSFER_RS26_PAIR(11);
				DOSFER_RS26_PAIR(12);
				#undef DOSFER_RS26_PAIR
			}
			#endif
		} else if (blockEccLen == 30) {
			#ifdef __WATCOMC__
			eccLocal[30]=0;
			dosferRs30PairAsm(dat,(uint16_t)datLen,rsStep,eccLocal);
			#else
			for (int d = 0; d < datLen; d++) {
				uint8_t factor = dat[d] ^ ecc[0];
				const uint8_t *row=rsStep+(unsigned)factor*DOSFER_RS_STRIDE;
				ecc[30]=0;
				#define DOSFER_RS30_PAIR(k) (*(uint16_t *)(ecc+(k)*2)=(uint16_t)(*(const uint16_t *)(ecc+(k)*2+1)^*(const uint16_t *)(row+(k)*2)))
				DOSFER_RS30_PAIR(0); DOSFER_RS30_PAIR(1); DOSFER_RS30_PAIR(2); DOSFER_RS30_PAIR(3);
				DOSFER_RS30_PAIR(4); DOSFER_RS30_PAIR(5); DOSFER_RS30_PAIR(6); DOSFER_RS30_PAIR(7);
				DOSFER_RS30_PAIR(8); DOSFER_RS30_PAIR(9); DOSFER_RS30_PAIR(10);DOSFER_RS30_PAIR(11);
				DOSFER_RS30_PAIR(12);DOSFER_RS30_PAIR(13);DOSFER_RS30_PAIR(14);
				#undef DOSFER_RS30_PAIR
			}
			#endif
		} else for (int d = 0; d < datLen; d++) {
			uint8_t factor = dat[d] ^ ecc[0];
			{
				const uint8_t *row=rsStep+(unsigned)factor*DOSFER_RS_STRIDE;
				for(int j=0;j+1<blockEccLen;j++)ecc[j]=(uint8_t)(ecc[j+1]^row[j]);
				ecc[blockEccLen-1]=row[blockEccLen-1];
			}
		}
		#ifdef __WATCOMC__
		if(version==40&&ecl==qrcodegen_Ecc_LOW&&numBlocks==25&&shortBlockDataLen==118) {
			dosferStrideCopy(dat,118,result+i,25);
			if(i>=19)result[2950+i-19]=dat[118];
			dosferStrideCopy(ecc,30,result+dataLen+i,25);
			dat+=datLen;continue;
		}
		#endif
		/* Portable equivalent of the assembly fast path, also used by the
		 * host-side QR oracle. */
		if(version==40&&ecl==qrcodegen_Ecc_LOW&&numBlocks==25&&shortBlockDataLen==118) {
			int k=i;
			for(int j=0;j<118;j++,k+=25)result[k]=dat[j];
			if(i>=19)result[2950+i-19]=dat[118];
		} else
		for (int j = 0, k = i; j < datLen; j++, k += numBlocks) {  // Copy data
			if (j == shortBlockDataLen)
				k -= numShortBlocks;
			result[k] = dat[j];
		}
		for (int j = 0, k = dataLen + i; j < blockEccLen; j++, k += numBlocks)  // Copy ECC
			result[k] = ecc[j];
		dat += datLen;
	}
}


/* Fixed V40-L hot path used by the 16-bit DOS sender.  It intentionally keeps
 * the proven degree-30 RS recurrence and the proven V40 interleave layout, but
 * removes all version/ECC/block-layout discovery from the per-frame path. */
static void dosferAddEccInterleaveV40L(uint8_t data[], uint8_t result[]) {
	const uint8_t *dat=data;
	static uint8_t
#ifdef __WATCOMC__
		__near
#endif
		ecc[
#ifdef __WATCOMC__
		25*32
#else
		32
#endif
		];
	int block,j;

	#ifdef __WATCOMC__
	dosferRestoreDgroup();
	#endif
	dosferPrepareRs(30);
	for(block=0;block<25;block++) {
		int datLen=block<19?118:119;
#ifdef __WATCOMC__
		{
			uint8_t __near *blockEcc=ecc+block*32;
			uint16_t quadLen=(uint16_t)(datLen&~3);
			memset(blockEcc,0,32);
			dosferRs30QuadAsm(dat,quadLen,dosferRsStep,blockEcc);
			dosferRs30PairAsm(dat+quadLen,(uint16_t)(datLen-quadLen),
				dosferRsStep,blockEcc);
		}
#else
		memset(ecc,0,sizeof(ecc));
		for(j=0;j<datLen;j++) {
			uint8_t factor=(uint8_t)(dat[j]^ecc[0]);
			const uint8_t *row=dosferRsStep+(unsigned)factor*DOSFER_RS_STRIDE;
			ecc[30]=0;
			for(int k=0;k<30;k++)ecc[k]=(uint8_t)(ecc[k+1]^row[k]);
		}
		for(j=0;j<118;j++)result[block+j*25]=dat[j];
		if(block>=19)result[2950+block-19]=dat[118];
		for(j=0;j<30;j++)result[2956+block+j*25]=ecc[j];
#endif
		dat+=datLen;
	}
#ifdef __WATCOMC__
	dosferInterleaveDataV40(data,result);
	dosferCopyV40LongData(data,result);
	dosferInterleaveEccV40(ecc,result+2956);
#endif
}

/* Pack one complete DOSfer frame into the fixed V40-L ECI-3/Byte data
 * codeword layout.  This is the packing half of the specialized encoder and
 * is exposed for the persistent streaming path. */
bool qrcodegen_dosferPackFrameV40L(const uint8_t frame[], uint16_t frameLen,
		uint8_t dataCodewords[]) {
	const int dataCapacity=2956;
	int i;
	uint8_t padByte;

	if(!frame||!dataCodewords||frameLen>2952)return false;
	dataCodewords[0]=0x70;
	dataCodewords[1]=0x34;
	dataCodewords[2]=(uint8_t)(frameLen>>8);
	dataCodewords[3]=(uint8_t)frameLen;
	if(frameLen)memcpy(dataCodewords+4,frame,frameLen);
	i=4+(int)frameLen;
	/* Every accepted short frame leaves at least one complete byte. Four zero
	 * terminator bits plus byte alignment therefore occupy exactly this byte. */
	if(i<dataCapacity)dataCodewords[i++]=0;
	padByte=0xEC;
	for(;i<dataCapacity;i++,padByte^=0xEC^0x11)
		dataCodewords[i]=padByte;
	return true;
}

/* Compute the 25 degree-30 V40-L ECC blocks in block-major order:
 * eccBlocks[block * 30 + eccByte].  The same proven two-input-byte RS
 * recurrence is used as by the canonical full encoder. */
void qrcodegen_dosferComputeEccBlocksV40L(const uint8_t dataCodewords[],
		uint8_t eccBlocks[]) {
	const uint8_t *dat=dataCodewords;
	static uint8_t
#ifdef __WATCOMC__
		__near
#endif
		ecc[32];
	int block,j;

	#ifdef __WATCOMC__
	dosferRestoreDgroup();
	#endif
	dosferPrepareRs(30);
	for(block=0;block<25;block++) {
		int datLen=block<19?118:119;
		memset(ecc,0,sizeof(ecc));
#ifdef __WATCOMC__
		{
			uint16_t quadLen=(uint16_t)(datLen&~3);
			dosferRs30QuadAsm(dat,quadLen,dosferRsStep,ecc);
			dosferRs30PairAsm(dat+quadLen,(uint16_t)(datLen-quadLen),
				dosferRsStep,ecc);
		}
#else
		for(j=0;j<datLen;j++) {
			uint8_t factor=(uint8_t)(dat[j]^ecc[0]);
			const uint8_t *row=dosferRsStep+(unsigned)factor*DOSFER_RS_STRIDE;
			ecc[30]=0;
			for(int k=0;k<30;k++)ecc[k]=(uint8_t)(ecc[k+1]^row[k]);
		}
#endif
		memcpy(eccBlocks+block*30,ecc,30);
		dat+=datLen;
	}
}

/* Compute three independent V40-L ECC streams in one block/quad traversal.
 * The mathematical recurrence and block-major output layout are identical to
 * three qrcodegen_dosferComputeEccBlocksV40L() calls. */
void qrcodegen_dosferComputeEccBlocks3V40L(
		const uint8_t *const dataCodewords[3],uint8_t *const eccBlocks[3]) {
#ifdef __WATCOMC__
	const uint8_t *red,*green,*blue;
	int block,channel,aligned,outputsAligned;

	if(!dataCodewords||!eccBlocks)return;
	for(channel=0;channel<3;channel++)
		if(!dataCodewords[channel]||!eccBlocks[channel])return;
	red=dataCodewords[0];green=dataCodewords[1];blue=dataCodewords[2];
	aligned=FP_OFF(red)==FP_OFF(green)&&FP_OFF(red)==FP_OFF(blue);
	outputsAligned=FP_OFF(eccBlocks[0])==FP_OFF(eccBlocks[1])&&
		FP_OFF(eccBlocks[0])==FP_OFF(eccBlocks[2]);
	dosferRestoreDgroup();
	dosferPrepareRs(30);
	for(block=0;block<25;block++) {
		uint16_t datLen=(uint16_t)(block<19?118:119);
		uint16_t quadLen=(uint16_t)(datLen&~3);
		memset(dosferRsEcc3,0,sizeof(dosferRsEcc3));
		if(aligned) {
			dosferRsBlue3=blue;
			dosferRs30Quad3Asm(red,green,quadLen);
		} else {
			dosferRs30QuadAsm(red,quadLen,dosferRsStep,dosferRsEcc3[0]);
			dosferRs30QuadAsm(green,quadLen,dosferRsStep,dosferRsEcc3[1]);
			dosferRs30QuadAsm(blue,quadLen,dosferRsStep,dosferRsEcc3[2]);
		}
		dosferRs30PairAsm(red+quadLen,(uint16_t)(datLen-quadLen),
			dosferRsStep,dosferRsEcc3[0]);
		dosferRs30PairAsm(green+quadLen,(uint16_t)(datLen-quadLen),
			dosferRsStep,dosferRsEcc3[1]);
		dosferRs30PairAsm(blue+quadLen,(uint16_t)(datLen-quadLen),
			dosferRsStep,dosferRsEcc3[2]);
		if(outputsAligned) {
			uint16_t outputOffset=(uint16_t)(block*30);
			dosferRsEccBlueOut3=eccBlocks[2]+outputOffset;
			dosferCopyEcc3Asm(eccBlocks[0]+outputOffset,
				eccBlocks[1]+outputOffset);
		} else {
			for(channel=0;channel<3;channel++)
				memcpy(eccBlocks[channel]+block*30,dosferRsEcc3[channel],30);
		}
		red+=datLen;green+=datLen;blue+=datLen;
	}
#else
	int channel;
	if(!dataCodewords||!eccBlocks)return;
	for(channel=0;channel<3;channel++)
		qrcodegen_dosferComputeEccBlocksV40L(dataCodewords[channel],
			eccBlocks[channel]);
#endif
}

bool qrcodegen_dosferEncodePrepacked3V40L(
		uint8_t *const dataCodewords[3],uint8_t *const result[3]) {
	uint8_t *eccBlocks[3];
	int channel;
	if(!dataCodewords||!result)return false;
	for(channel=0;channel<3;channel++) {
		if(!dataCodewords[channel]||!result[channel])return false;
		eccBlocks[channel]=dataCodewords[channel]+2956;
	}
	qrcodegen_dosferComputeEccBlocks3V40L(
		(const uint8_t *const *)dataCodewords,eccBlocks);
#ifdef __WATCOMC__
	for(channel=0;channel<3;channel++) {
		dosferInterleaveDataV40(dataCodewords[channel],result[channel]);
		dosferCopyV40LongData(dataCodewords[channel],result[channel]);
		dosferInterleaveEcc30V40(eccBlocks[channel],result[channel]+2956);
	}
#else
	for(channel=0;channel<3;channel++) {
		int block,row;
		const uint8_t *dat=dataCodewords[channel];
		for(row=0;row<118;row++) {
			int offset=0;
			for(block=0;block<25;block++) {
				result[channel][row*25+block]=dat[offset+row];
				offset+=block<19?118:119;
			}
		}
		for(block=19;block<25;block++) {
			int offset=19*118+(block-19)*119;
			result[channel][2950+block-19]=dat[offset+118];
		}
		for(row=0;row<30;row++)for(block=0;block<25;block++)
			result[channel][2956+row*25+block]=eccBlocks[channel][block*30+row];
	}
#endif
	return true;
}

/* Encode one complete DOSfer transport frame as fixed QR V40-L + ECI 3.
 * This is byte-for-byte equivalent to qrcodegen_encodeBinaryAligned() with
 * version 40, ECC L and a fixed mask, but avoids segment construction,
 * version search, ECC selection and generic block-layout branches.
 *
 * `workspace` is the normal V40 QR buffer.  On full rendering it receives
 * the packed matrix.  In codewords-only mode its contents are unspecified.
 * `codewords` always receives the 3706 interleaved V40-L codewords. */
bool qrcodegen_dosferEncodeFrameV40L(const uint8_t frame[], uint16_t frameLen,
		uint8_t codewords[], uint8_t workspace[], enum qrcodegen_Mask mask,
		bool codewordsOnly) {
	DOSFER_PROFILE_START;

	if(!frame||!codewords||!workspace||frameLen>2952||
			(int)mask<0||(int)mask>7)return false;
	if(!qrcodegen_dosferPackFrameV40L(frame,frameLen,workspace))return false;
	DOSFER_PROFILE_MARK(0);

	dosferAddEccInterleaveV40L(workspace,codewords);
	DOSFER_PROFILE_MARK(1);
	if(codewordsOnly)return true;
	if(!dosferPrepareMatrixCache(40,qrcodegen_Ecc_LOW,mask))return false;
	memcpy(workspace,dosferFunctionTemplate,
		(size_t)qrcodegen_BUFFER_LEN_FOR_VERSION(40));
	DOSFER_PROFILE_MARK(2);
	dosferDrawCodewordsCached(codewords,3706,workspace);
	DOSFER_PROFILE_MARK(3);
	DOSFER_PROFILE_MARK(4);
	DOSFER_PROFILE_MARK(5);
	return true;
}

bool qrcodegen_dosferCorrectXorV40L(const uint8_t encodedXor[],uint16_t xorCount,
        const uint8_t protocolHeaderXor[48],uint8_t result[]) {
    static uint8_t
#ifdef __WATCOMC__
        __near
#endif
        data[118];
    static uint8_t
#ifdef __WATCOMC__
        __near
#endif
        ecc[32];
    uint16_t i;
#ifdef DOSFER_PROFILE
    u32 profileStart=timer_ticks(),profileNow;
#endif
    if(!encodedXor||!xorCount||!protocolHeaderXor||!result)return false;
    memset(data,0,sizeof(data));memset(ecc,0,sizeof(ecc));
    /* Even source counts cancel the common ECI 3 + Byte mode + length prefix;
     * odd source counts retain it.  The caller supplies only the correction
     * from XOR(source DOSfer headers) to the desired DOSfer header. */
    if(!(xorCount&1)) {
        data[0]=0x70;data[1]=0x34;data[2]=0x0B;data[3]=0x88;
    }
    memcpy(data+4,protocolHeaderXor,48);dosferPrepareRs(30);
#ifdef __WATCOMC__
    dosferRs30QuadAsm(data,116,dosferRsStep,ecc);
    dosferRs30PairAsm(data+116,2,dosferRsStep,ecc);
#else
    for(i=0;i<118;i++){uint8_t factor=data[i]^ecc[0];const uint8_t *row=dosferRsStep+(unsigned)factor*DOSFER_RS_STRIDE;
        ecc[30]=0;for(int j=0;j<30;j++)ecc[j]=ecc[j+1]^row[j];}
#endif
#ifdef DOSFER_PROFILE
    profileNow=timer_ticks();dosferQrProfileTicks[1]+=profileNow-profileStart;profileStart=profileNow;
#endif
    if(result!=encodedXor)memcpy(result,encodedXor,3706);
    for(i=0;i<52;i++)result[i*25]^=data[i];
    for(i=0;i<30;i++)result[2956+i*25]^=ecc[i];
#ifdef DOSFER_PROFILE
    dosferQrProfileTicks[0]+=timer_ticks()-profileStart;
#endif
    return true;
}

bool qrcodegen_dosferDeriveXorV40L(const uint8_t encodedLeft[],const uint8_t encodedRight[],
        const uint8_t protocolHeaderXor[48],uint8_t result[]) {
    uint16_t i;
    if(!encodedLeft||!encodedRight||!protocolHeaderXor||!result)return false;
    for(i=0;i+4<=3706;i+=4)
        *(uint32_t *)(result+i)=*(const uint32_t *)(encodedLeft+i)^
            *(const uint32_t *)(encodedRight+i);
    for(;i<3706;i++)result[i]=encodedLeft[i]^encodedRight[i];
    return qrcodegen_dosferCorrectXorV40L(result,2,protocolHeaderXor,result);
}

bool qrcodegen_dosferDeriveXor3V40L(const uint8_t encodedA[],const uint8_t encodedB[],
        const uint8_t encodedC[],const uint8_t protocolHeaderXor[48],uint8_t result[]) {
    uint16_t i;
    if(!encodedA||!encodedB||!encodedC||!protocolHeaderXor||!result)return false;
    /* Two 32-bit XORs derive the same codeword position for all three source
     * streams.  The odd source count preserves the common QR prefix. */
    for(i=0;i+4<=3706;i+=4)
        *(uint32_t *)(result+i)=*(const uint32_t *)(encodedA+i)^
            *(const uint32_t *)(encodedB+i)^*(const uint32_t *)(encodedC+i);
    for(;i<3706;i++)result[i]=encodedA[i]^encodedB[i]^encodedC[i];
    return qrcodegen_dosferCorrectXorV40L(result,3,protocolHeaderXor,result);
}

bool qrcodegen_dosferEncodePrepackedV40L(uint8_t dataCodewords[],uint8_t result[]) {
	/* V40-L has exactly 2956 data codewords. The caller supplies the fixed
	 * ECI-3/Byte header followed by a complete 2952-byte DOSfer frame. */
#ifdef DOSFER_PROFILE
	u32 profileStart=timer_ticks();
#endif
	dosferAddEccInterleaveV40L(dataCodewords,result);
#ifdef DOSFER_PROFILE
	dosferQrProfileTicks[1]+=timer_ticks()-profileStart;
#endif
	return true;
}


// Returns the number of 8-bit codewords that can be used for storing data (not ECC),
// for the given version number and error correction level. The result is in the range [9, 2956].
testable int getNumDataCodewords(int version, enum qrcodegen_Ecc ecl) {
	int v = version, e = (int)ecl;
	assert(0 <= e && e < 4);
	return getNumRawDataModules(v) / 8
		- ECC_CODEWORDS_PER_BLOCK    [e][v]
		* NUM_ERROR_CORRECTION_BLOCKS[e][v];
}


// Returns the number of data bits that can be stored in a QR Code of the given version number, after
// all function modules are excluded. This includes remainder bits, so it might not be a multiple of 8.
// The result is in the range [208, 29648]. This could be implemented as a 40-entry lookup table.
testable int getNumRawDataModules(int ver) {
	assert(qrcodegen_VERSION_MIN <= ver && ver <= qrcodegen_VERSION_MAX);
	int result = (16 * ver + 128) * ver + 64;
	if (ver >= 2) {
		int numAlign = ver / 7 + 2;
		result -= (25 * numAlign - 10) * numAlign - 55;
		if (ver >= 7)
			result -= 36;
	}
	assert(208 <= result && result <= 29648);
	return result;
}



/*---- Reed-Solomon ECC generator functions ----*/

// Computes a Reed-Solomon ECC generator polynomial for the given degree, storing in result[0 : degree].
// This could be implemented as a lookup table over all possible parameter values, instead of as an algorithm.
testable void reedSolomonComputeDivisor(int degree, uint8_t result[]) {
	assert(1 <= degree && degree <= qrcodegen_REED_SOLOMON_DEGREE_MAX);
	// Polynomial coefficients are stored from highest to lowest power, excluding the leading term which is always 1.
	// For example the polynomial x^3 + 255x^2 + 8x + 93 is stored as the uint8 array {255, 8, 93}.
	memset(result, 0, (size_t)degree * sizeof(result[0]));
	result[degree - 1] = 1;  // Start off with the monomial x^0
	
	// Compute the product polynomial (x - r^0) * (x - r^1) * (x - r^2) * ... * (x - r^{degree-1}),
	// drop the highest monomial term which is always 1x^degree.
	// Note that r = 0x02, which is a generator element of this field GF(2^8/0x11D).
	uint8_t root = 1;
	for (int i = 0; i < degree; i++) {
		// Multiply the current product by (x - r^i)
		for (int j = 0; j < degree; j++) {
			result[j] = reedSolomonMultiply(result[j], root);
			if (j + 1 < degree)
				result[j] ^= result[j + 1];
		}
		root = reedSolomonMultiply(root, 0x02);
	}
}


// Computes the Reed-Solomon error correction codeword for the given data and divisor polynomials.
// The remainder when data[0 : dataLen] is divided by divisor[0 : degree] is stored in result[0 : degree].
// All polynomials are in big endian, and the generator has an implicit leading 1 term.
testable void reedSolomonComputeRemainder(const uint8_t data[], int dataLen,
		const uint8_t generator[], int degree, uint8_t result[]) {
	assert(1 <= degree && degree <= qrcodegen_REED_SOLOMON_DEGREE_MAX);
	memset(result, 0, (size_t)degree * sizeof(result[0]));
	for (int i = 0; i < dataLen; i++) {  // Polynomial division
		uint8_t factor = data[i] ^ result[0];
		memmove(&result[0], &result[1], (size_t)(degree - 1) * sizeof(result[0]));
		result[degree - 1] = 0;
		for (int j = 0; j < degree; j++)
			result[j] ^= reedSolomonMultiply(generator[j], factor);
	}
}

#undef qrcodegen_REED_SOLOMON_DEGREE_MAX


// Returns the product of the two given field elements modulo GF(2^8/0x11D).
// All inputs are valid. This could be implemented as a 256*256 lookup table.
testable uint8_t reedSolomonMultiply(uint8_t x, uint8_t y) {
	if (x == 0 || y == 0)
		return 0;
	if (!dosferGfReady)
		dosferInitGf();
	return dosferGfExp[(int)dosferGfLog[x] + (int)dosferGfLog[y]];
}



/*---- Drawing function modules ----*/

// Clears the given QR Code grid with light modules for the given
// version's size, then marks every function module as dark.
testable void initializeFunctionModules(int version, uint8_t qrcode[]) {
	// Initialize QR Code
	int qrsize = version * 4 + 17;
	memset(qrcode, 0, (size_t)((qrsize * qrsize + 7) / 8 + 1) * sizeof(qrcode[0]));
	qrcode[0] = (uint8_t)qrsize;
	
	// Fill horizontal and vertical timing patterns
	fillRectangle(6, 0, 1, qrsize, qrcode);
	fillRectangle(0, 6, qrsize, 1, qrcode);
	
	// Fill 3 finder patterns (all corners except bottom right) and format bits
	fillRectangle(0, 0, 9, 9, qrcode);
	fillRectangle(qrsize - 8, 0, 8, 9, qrcode);
	fillRectangle(0, qrsize - 8, 9, 8, qrcode);
	
	// Fill numerous alignment patterns
	uint8_t alignPatPos[7];
	int numAlign = getAlignmentPatternPositions(version, alignPatPos);
	for (int i = 0; i < numAlign; i++) {
		for (int j = 0; j < numAlign; j++) {
			// Don't draw on the three finder corners
			if (!((i == 0 && j == 0) || (i == 0 && j == numAlign - 1) || (i == numAlign - 1 && j == 0)))
				fillRectangle(alignPatPos[i] - 2, alignPatPos[j] - 2, 5, 5, qrcode);
		}
	}
	
	// Fill version blocks
	if (version >= 7) {
		fillRectangle(qrsize - 11, 0, 3, 6, qrcode);
		fillRectangle(0, qrsize - 11, 6, 3, qrcode);
	}
}


// Draws light function modules and possibly some dark modules onto the given QR Code, without changing
// non-function modules. This does not draw the format bits. This requires all function modules to be previously
// marked dark (namely by initializeFunctionModules()), because this may skip redrawing dark function modules.
static void drawLightFunctionModules(uint8_t qrcode[], int version) {
	// Draw horizontal and vertical timing patterns
	int qrsize = qrcodegen_getSize(qrcode);
	for (int i = 7; i < qrsize - 7; i += 2) {
		setModuleBounded(qrcode, 6, i, false);
		setModuleBounded(qrcode, i, 6, false);
	}
	
	// Draw 3 finder patterns (all corners except bottom right; overwrites some timing modules)
	for (int dy = -4; dy <= 4; dy++) {
		for (int dx = -4; dx <= 4; dx++) {
			int dist = abs(dx);
			if (abs(dy) > dist)
				dist = abs(dy);
			if (dist == 2 || dist == 4) {
				setModuleUnbounded(qrcode, 3 + dx, 3 + dy, false);
				setModuleUnbounded(qrcode, qrsize - 4 + dx, 3 + dy, false);
				setModuleUnbounded(qrcode, 3 + dx, qrsize - 4 + dy, false);
			}
		}
	}
	
	// Draw numerous alignment patterns
	uint8_t alignPatPos[7];
	int numAlign = getAlignmentPatternPositions(version, alignPatPos);
	for (int i = 0; i < numAlign; i++) {
		for (int j = 0; j < numAlign; j++) {
			if ((i == 0 && j == 0) || (i == 0 && j == numAlign - 1) || (i == numAlign - 1 && j == 0))
				continue;  // Don't draw on the three finder corners
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++)
					setModuleBounded(qrcode, alignPatPos[i] + dx, alignPatPos[j] + dy, dx == 0 && dy == 0);
			}
		}
	}
	
	// Draw version blocks
	if (version >= 7) {
		// Calculate error correction code and pack bits
		int rem = version;  // version is uint6, in the range [7, 40]
		for (int i = 0; i < 12; i++)
			rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
		long bits = (long)version << 12 | rem;  // uint18
		assert(bits >> 18 == 0);
		
		// Draw two copies
		for (int i = 0; i < 6; i++) {
			for (int j = 0; j < 3; j++) {
				int k = qrsize - 11 + j;
				setModuleBounded(qrcode, k, i, (bits & 1) != 0);
				setModuleBounded(qrcode, i, k, (bits & 1) != 0);
				bits >>= 1;
			}
		}
	}
}


// Draws two copies of the format bits (with its own error correction code) based
// on the given mask and error correction level. This always draws all modules of
// the format bits, unlike drawLightFunctionModules() which might skip dark modules.
static void drawFormatBits(enum qrcodegen_Ecc ecl, enum qrcodegen_Mask mask, uint8_t qrcode[]) {
	// Calculate error correction code and pack bits
	assert(0 <= (int)mask && (int)mask <= 7);
	static const int table[] = {1, 0, 3, 2};
	int data = table[(int)ecl] << 3 | (int)mask;  // errCorrLvl is uint2, mask is uint3
	int rem = data;
	for (int i = 0; i < 10; i++)
		rem = (rem << 1) ^ ((rem >> 9) * 0x537);
	int bits = (data << 10 | rem) ^ 0x5412;  // uint15
	assert(bits >> 15 == 0);
	
	// Draw first copy
	for (int i = 0; i <= 5; i++)
		setModuleBounded(qrcode, 8, i, getBit(bits, i));
	setModuleBounded(qrcode, 8, 7, getBit(bits, 6));
	setModuleBounded(qrcode, 8, 8, getBit(bits, 7));
	setModuleBounded(qrcode, 7, 8, getBit(bits, 8));
	for (int i = 9; i < 15; i++)
		setModuleBounded(qrcode, 14 - i, 8, getBit(bits, i));
	
	// Draw second copy
	int qrsize = qrcodegen_getSize(qrcode);
	for (int i = 0; i < 8; i++)
		setModuleBounded(qrcode, qrsize - 1 - i, 8, getBit(bits, i));
	for (int i = 8; i < 15; i++)
		setModuleBounded(qrcode, 8, qrsize - 15 + i, getBit(bits, i));
	setModuleBounded(qrcode, 8, qrsize - 8, true);  // Always dark
}


// Calculates and stores an ascending list of positions of alignment patterns
// for this version number, returning the length of the list (in the range [0,7]).
// Each position is in the range [0,177), and are used on both the x and y axes.
// This could be implemented as lookup table of 40 variable-length lists of unsigned bytes.
testable int getAlignmentPatternPositions(int version, uint8_t result[7]) {
	if (version == 1)
		return 0;
	int numAlign = version / 7 + 2;
	int step = (version * 8 + numAlign * 3 + 5) / (numAlign * 4 - 4) * 2;
	for (int i = numAlign - 1, pos = version * 4 + 10; i >= 1; i--, pos -= step)
		result[i] = (uint8_t)pos;
	result[0] = 6;
	return numAlign;
}


// Sets every module in the range [left : left + width] * [top : top + height] to dark.
static void fillRectangle(int left, int top, int width, int height, uint8_t qrcode[]) {
	for (int dy = 0; dy < height; dy++) {
		for (int dx = 0; dx < width; dx++)
			setModuleBounded(qrcode, left + dx, top + dy, true);
	}
}


/* DOSfer fixed-version cache. The generic implementation repeatedly discovers
 * the same function/data module layout and recomputes mask 0 for every frame.
 * These heap buffers keep near DGROUP below 64K in the 16-bit large model. */
#define DOSFER_CACHE_MAX_VERSION 40
static bool dosferPrepareMatrixCache(int version, enum qrcodegen_Ecc ecl, enum qrcodegen_Mask mask) {
#ifdef DOSFER_DISABLE_MATRIX_CACHE
	(void)version;(void)ecl;(void)mask;
	return false;
#else
	const size_t neededLen = qrcodegen_BUFFER_LEN_FOR_VERSION(version);
	const size_t neededBits = (size_t)(getNumRawDataModules(version) / 8 * 8);
	int qrsize, rawBits, i, right, vert, j, x, y, index;
	if (version > DOSFER_CACHE_MAX_VERSION)
		return false;
	if (dosferFunctionTemplate == NULL || dosferDataModule == NULL ||
			neededLen > dosferTemplateCapacity || neededBits > dosferMapCapacity) {
		free(dosferFunctionTemplate);free(dosferDataModule);
		dosferFunctionTemplate = malloc(neededLen);
		dosferDataModule = malloc(neededBits * sizeof(dosferDataModule[0]));
		if (dosferFunctionTemplate == NULL ||
				dosferDataModule == NULL) {
			free(dosferFunctionTemplate);
			free(dosferDataModule);
			dosferFunctionTemplate = NULL;
			dosferDataModule = NULL;
			dosferTemplateCapacity=0;dosferMapCapacity=0;
			return false;
		}
		dosferTemplateCapacity=neededLen;dosferMapCapacity=neededBits;
		dosferCacheVersion=0;dosferCacheEcl=-1;dosferCacheMask=-1;
	}
	if (dosferCacheVersion == version && dosferCacheEcl == (int)ecl && dosferCacheMask == (int)mask)
		return true;
	initializeFunctionModules(version, dosferFunctionTemplate);
	qrsize = version * 4 + 17;
	rawBits = getNumRawDataModules(version) / 8 * 8;
	i = 0;
	for (right = qrsize - 1; right >= 1; right -= 2) {
		if (right == 6) right = 5;
		for (vert = 0; vert < qrsize; vert++) for (j = 0; j < 2; j++) {
			x = right - j;
			y = (((right + 1) & 2) == 0) ? qrsize - 1 - vert : vert;
			index = y * qrsize + x;
			if (!(dosferFunctionTemplate[(index >> 3) + 1] & (1 << (index & 7))) && i < rawBits) {
				dosferDataModule[i] = (uint16_t)index;
				i++;
			}
		}
	}
	if (i != rawBits)
		return false;
	for (y = 0; y < qrsize; y++) for (x = 0; x < qrsize; x++) {
		bool invert;
		index = y * qrsize + x;
		switch ((int)mask) {
			case 0: invert=(x+y)%2==0;break;
			case 1: invert=y%2==0;break;
			case 2: invert=x%3==0;break;
			case 3: invert=(x+y)%3==0;break;
			case 4: invert=(x/3+y/2)%2==0;break;
			case 5: invert=x*y%2+x*y%3==0;break;
			case 6: invert=(x*y%2+x*y%3)%2==0;break;
			case 7: invert=((x+y)%2+x*y%3)%2==0;break;
			default: return false;
		}
		if (invert && !(dosferFunctionTemplate[(index >> 3) + 1] & (1 << (index & 7))))
			dosferFunctionTemplate[(index >> 3) + 1] |= (uint8_t)(1 << (index & 7));
	}
	drawLightFunctionModules(dosferFunctionTemplate, version);
	drawFormatBits(ecl, mask, dosferFunctionTemplate);
	dosferCacheVersion = version;
	dosferCacheEcl = (int)ecl;
	dosferCacheMask = (int)mask;
	dosferCacheDataBits = rawBits;
	return true;
#endif
}

static void dosferDrawCodewordsCached(const uint8_t data[], int dataLen, uint8_t qrcode[]) {
	int i,base=0;uint8_t v;uint16_t pos;
	if(dataLen*8>dosferCacheDataBits)dataLen=dosferCacheDataBits/8;
	for(i=0;i<dataLen;i++,base+=8){v=data[i];
#define DRAW_BIT(k,b) if(v&(b)){pos=dosferDataModule[base+(k)];qrcode[(pos>>3)+1]^=(uint8_t)(1<<(pos&7));}
		DRAW_BIT(0,0x80) DRAW_BIT(1,0x40) DRAW_BIT(2,0x20) DRAW_BIT(3,0x10)
		DRAW_BIT(4,0x08) DRAW_BIT(5,0x04) DRAW_BIT(6,0x02) DRAW_BIT(7,0x01)
#undef DRAW_BIT
	}
}

int qrcodegen_dosferPlacementBits(void) { return dosferCacheDataBits; }
uint16_t *qrcodegen_dosferTakePlacementModules(void) {
	uint16_t *result=dosferDataModule;
	dosferDataModule=NULL;
	dosferMapCapacity=0;
	return result;
}
void qrcodegen_dosferReleaseMatrixCache(void) {
	free(dosferFunctionTemplate);free(dosferDataModule);
	dosferFunctionTemplate=NULL;dosferDataModule=NULL;
	dosferTemplateCapacity=dosferMapCapacity=0;dosferCacheVersion=0;
	dosferCacheEcl=-1;dosferCacheMask=-1;dosferCacheDataBits=0;
}



/*---- Drawing data modules and masking ----*/

// Draws the raw codewords (including data and ECC) onto the given QR Code. This requires the initial state of
// the QR Code to be dark at function modules and light at codeword modules (including unused remainder bits).
static void drawCodewords(const uint8_t data[], int dataLen, uint8_t qrcode[]) {
	int qrsize = qrcodegen_getSize(qrcode);
	int i = 0;  // Bit index into the data
	// Do the funny zigzag scan
	for (int right = qrsize - 1; right >= 1; right -= 2) {  // Index of right column in each column pair
		if (right == 6)
			right = 5;
		for (int vert = 0; vert < qrsize; vert++) {  // Vertical counter
			for (int j = 0; j < 2; j++) {
				int x = right - j;  // Actual x coordinate
				bool upward = ((right + 1) & 2) == 0;
				int y = upward ? qrsize - 1 - vert : vert;  // Actual y coordinate
				if (!getModuleBounded(qrcode, x, y) && i < dataLen * 8) {
					bool dark = getBit(data[i >> 3], 7 - (i & 7));
					setModuleBounded(qrcode, x, y, dark);
					i++;
				}
				// If this QR Code has any remainder bits (0 to 7), they were assigned as
				// 0/false/light by the constructor and are left unchanged by this method
			}
		}
	}
	assert(i == dataLen * 8);
}


// XORs the codeword modules in this QR Code with the given mask pattern
// and given pattern of function modules. The codeword bits must be drawn
// before masking. Due to the arithmetic of XOR, calling applyMask() with
// the same mask value a second time will undo the mask. A final well-formed
// QR Code needs exactly one (not zero, two, etc.) mask applied.
static void applyMask(const uint8_t functionModules[], uint8_t qrcode[], enum qrcodegen_Mask mask) {
	assert(0 <= (int)mask && (int)mask <= 7);  // Disallows qrcodegen_Mask_AUTO
	int qrsize = qrcodegen_getSize(qrcode);
	for (int y = 0; y < qrsize; y++) {
		for (int x = 0; x < qrsize; x++) {
			if (getModuleBounded(functionModules, x, y))
				continue;
			bool invert;
			switch ((int)mask) {
				case 0:  invert = (x + y) % 2 == 0;                    break;
				case 1:  invert = y % 2 == 0;                          break;
				case 2:  invert = x % 3 == 0;                          break;
				case 3:  invert = (x + y) % 3 == 0;                    break;
				case 4:  invert = (x / 3 + y / 2) % 2 == 0;            break;
				case 5:  invert = x * y % 2 + x * y % 3 == 0;          break;
				case 6:  invert = (x * y % 2 + x * y % 3) % 2 == 0;    break;
				case 7:  invert = ((x + y) % 2 + x * y % 3) % 2 == 0;  break;
				default:  assert(false);  return;
			}
			bool val = getModuleBounded(qrcode, x, y);
			setModuleBounded(qrcode, x, y, val ^ invert);
		}
	}
}


// Calculates and returns the penalty score based on state of the given QR Code's current modules.
// This is used by the automatic mask choice algorithm to find the mask pattern that yields the lowest score.
testable long getPenaltyScore(const uint8_t qrcode[]) {
	int qrsize = qrcodegen_getSize(qrcode);
	long result = 0;
	
	// Adjacent modules in row having same color, and finder-like patterns
	for (int y = 0; y < qrsize; y++) {
		bool runColor = false;
		int runX = 0;
		int runHistory[7] = {0};
		for (int x = 0; x < qrsize; x++) {
			if (getModuleBounded(qrcode, x, y) == runColor) {
				runX++;
				if (runX == 5)
					result += PENALTY_N1;
				else if (runX > 5)
					result++;
			} else {
				finderPenaltyAddHistory(runX, runHistory, qrsize);
				if (!runColor)
					result += finderPenaltyCountPatterns(runHistory, qrsize) * PENALTY_N3;
				runColor = getModuleBounded(qrcode, x, y);
				runX = 1;
			}
		}
		result += finderPenaltyTerminateAndCount(runColor, runX, runHistory, qrsize) * PENALTY_N3;
	}
	// Adjacent modules in column having same color, and finder-like patterns
	for (int x = 0; x < qrsize; x++) {
		bool runColor = false;
		int runY = 0;
		int runHistory[7] = {0};
		for (int y = 0; y < qrsize; y++) {
			if (getModuleBounded(qrcode, x, y) == runColor) {
				runY++;
				if (runY == 5)
					result += PENALTY_N1;
				else if (runY > 5)
					result++;
			} else {
				finderPenaltyAddHistory(runY, runHistory, qrsize);
				if (!runColor)
					result += finderPenaltyCountPatterns(runHistory, qrsize) * PENALTY_N3;
				runColor = getModuleBounded(qrcode, x, y);
				runY = 1;
			}
		}
		result += finderPenaltyTerminateAndCount(runColor, runY, runHistory, qrsize) * PENALTY_N3;
	}
	
	// 2*2 blocks of modules having same color
	for (int y = 0; y < qrsize - 1; y++) {
		for (int x = 0; x < qrsize - 1; x++) {
			bool  color = getModuleBounded(qrcode, x, y);
			if (  color == getModuleBounded(qrcode, x + 1, y) &&
			      color == getModuleBounded(qrcode, x, y + 1) &&
			      color == getModuleBounded(qrcode, x + 1, y + 1))
				result += PENALTY_N2;
		}
	}
	
	// Balance of dark and light modules
	int dark = 0;
	for (int y = 0; y < qrsize; y++) {
		for (int x = 0; x < qrsize; x++) {
			if (getModuleBounded(qrcode, x, y))
				dark++;
		}
	}
	int total = qrsize * qrsize;  // Note that size is odd, so dark/total != 1/2
	// Compute the smallest integer k >= 0 such that (45-5k)% <= dark/total <= (55+5k)%
	int k = (int)((labs(dark * 20L - total * 10L) + total - 1) / total) - 1;
	assert(0 <= k && k <= 9);
	result += k * PENALTY_N4;
	assert(0 <= result && result <= 2568888L);  // Non-tight upper bound based on default values of PENALTY_N1, ..., N4
	return result;
}


// Can only be called immediately after a light run is added, and
// returns either 0, 1, or 2. A helper function for getPenaltyScore().
static int finderPenaltyCountPatterns(const int runHistory[7], int qrsize) {
	int n = runHistory[1];
	assert(n <= qrsize * 3);  (void)qrsize;
	bool core = n > 0 && runHistory[2] == n && runHistory[3] == n * 3 && runHistory[4] == n && runHistory[5] == n;
	// The maximum QR Code size is 177, hence the dark run length n <= 177.
	// Arithmetic is promoted to int, so n*4 will not overflow.
	return (core && runHistory[0] >= n * 4 && runHistory[6] >= n ? 1 : 0)
	     + (core && runHistory[6] >= n * 4 && runHistory[0] >= n ? 1 : 0);
}


// Must be called at the end of a line (row or column) of modules. A helper function for getPenaltyScore().
static int finderPenaltyTerminateAndCount(bool currentRunColor, int currentRunLength, int runHistory[7], int qrsize) {
	if (currentRunColor) {  // Terminate dark run
		finderPenaltyAddHistory(currentRunLength, runHistory, qrsize);
		currentRunLength = 0;
	}
	currentRunLength += qrsize;  // Add light border to final run
	finderPenaltyAddHistory(currentRunLength, runHistory, qrsize);
	return finderPenaltyCountPatterns(runHistory, qrsize);
}


// Pushes the given value to the front and drops the last value. A helper function for getPenaltyScore().
static void finderPenaltyAddHistory(int currentRunLength, int runHistory[7], int qrsize) {
	if (runHistory[0] == 0)
		currentRunLength += qrsize;  // Add light border to initial run
	memmove(&runHistory[1], &runHistory[0], 6 * sizeof(runHistory[0]));
	runHistory[0] = currentRunLength;
}



/*---- Basic QR Code information ----*/

// Public function - see documentation comment in header file.
int qrcodegen_getSize(const uint8_t qrcode[]) {
	assert(qrcode != NULL);
	int result = qrcode[0];
	assert((qrcodegen_VERSION_MIN * 4 + 17) <= result
		&& result <= (qrcodegen_VERSION_MAX * 4 + 17));
	return result;
}


// Public function - see documentation comment in header file.
bool qrcodegen_getModule(const uint8_t qrcode[], int x, int y) {
	assert(qrcode != NULL);
	int qrsize = qrcode[0];
	return (0 <= x && x < qrsize && 0 <= y && y < qrsize) && getModuleBounded(qrcode, x, y);
}


// Returns the color of the module at the given coordinates, which must be in bounds.
testable bool getModuleBounded(const uint8_t qrcode[], int x, int y) {
	int qrsize = qrcode[0];
	assert(21 <= qrsize && qrsize <= 177 && 0 <= x && x < qrsize && 0 <= y && y < qrsize);
	int index = y * qrsize + x;
	return getBit(qrcode[(index >> 3) + 1], index & 7);
}


// Sets the color of the module at the given coordinates, which must be in bounds.
testable void setModuleBounded(uint8_t qrcode[], int x, int y, bool isDark) {
	int qrsize = qrcode[0];
	assert(21 <= qrsize && qrsize <= 177 && 0 <= x && x < qrsize && 0 <= y && y < qrsize);
	int index = y * qrsize + x;
	int bitIndex = index & 7;
	int byteIndex = (index >> 3) + 1;
	if (isDark)
		qrcode[byteIndex] |= 1 << bitIndex;
	else
		qrcode[byteIndex] &= (1 << bitIndex) ^ 0xFF;
}


// Sets the color of the module at the given coordinates, doing nothing if out of bounds.
testable void setModuleUnbounded(uint8_t qrcode[], int x, int y, bool isDark) {
	int qrsize = qrcode[0];
	if (0 <= x && x < qrsize && 0 <= y && y < qrsize)
		setModuleBounded(qrcode, x, y, isDark);
}


// Returns true iff the i'th bit of x is set to 1. Requires x >= 0 and 0 <= i <= 14.
static bool getBit(int x, int i) {
	return ((x >> i) & 1) != 0;
}



/*---- Segment handling ----*/

// Public function - see documentation comment in header file.
bool qrcodegen_isNumeric(const char *text) {
	assert(text != NULL);
	for (; *text != '\0'; text++) {
		if (*text < '0' || *text > '9')
			return false;
	}
	return true;
}


// Public function - see documentation comment in header file.
bool qrcodegen_isAlphanumeric(const char *text) {
	assert(text != NULL);
	for (; *text != '\0'; text++) {
		if (strchr(ALPHANUMERIC_CHARSET, *text) == NULL)
			return false;
	}
	return true;
}


// Public function - see documentation comment in header file.
size_t qrcodegen_calcSegmentBufferSize(enum qrcodegen_Mode mode, size_t numChars) {
	int temp = calcSegmentBitLength(mode, numChars);
	if (temp == LENGTH_OVERFLOW)
		return SIZE_MAX;
	assert(0 <= temp && temp <= INT16_MAX);
	return ((size_t)temp + 7) / 8;
}


// Returns the number of data bits needed to represent a segment
// containing the given number of characters using the given mode. Notes:
// - Returns LENGTH_OVERFLOW on failure, i.e. numChars > INT16_MAX
//   or the number of needed bits exceeds INT16_MAX (i.e. 32767).
// - Otherwise, all valid results are in the range [0, INT16_MAX].
// - For byte mode, numChars measures the number of bytes, not Unicode code points.
// - For ECI mode, numChars must be 0, and the worst-case number of bits is returned.
//   An actual ECI segment can have shorter data. For non-ECI modes, the result is exact.
testable int calcSegmentBitLength(enum qrcodegen_Mode mode, size_t numChars) {
	// All calculations are designed to avoid overflow on all platforms
	if (numChars > (unsigned int)INT16_MAX)
		return LENGTH_OVERFLOW;
	long result = (long)numChars;
	if (mode == qrcodegen_Mode_NUMERIC)
		result = (result * 10 + 2) / 3;  // ceil(10/3 * n)
	else if (mode == qrcodegen_Mode_ALPHANUMERIC)
		result = (result * 11 + 1) / 2;  // ceil(11/2 * n)
	else if (mode == qrcodegen_Mode_BYTE)
		result *= 8;
	else if (mode == qrcodegen_Mode_KANJI)
		result *= 13;
	else if (mode == qrcodegen_Mode_ECI && numChars == 0)
		result = 3 * 8;
	else {  // Invalid argument
		assert(false);
		return LENGTH_OVERFLOW;
	}
	assert(result >= 0);
	if (result > INT16_MAX)
		return LENGTH_OVERFLOW;
	return (int)result;
}


// Public function - see documentation comment in header file.
struct qrcodegen_Segment qrcodegen_makeBytes(const uint8_t data[], size_t len, uint8_t buf[]) {
	assert(data != NULL || len == 0);
	struct qrcodegen_Segment result;
	result.mode = qrcodegen_Mode_BYTE;
	result.bitLength = calcSegmentBitLength(result.mode, len);
	assert(result.bitLength != LENGTH_OVERFLOW);
	result.numChars = (int)len;
	if (len > 0)
		memcpy(buf, data, len * sizeof(buf[0]));
	result.data = buf;
	return result;
}


// Public function - see documentation comment in header file.
struct qrcodegen_Segment qrcodegen_makeNumeric(const char *digits, uint8_t buf[]) {
	assert(digits != NULL);
	struct qrcodegen_Segment result;
	size_t len = strlen(digits);
	result.mode = qrcodegen_Mode_NUMERIC;
	int bitLen = calcSegmentBitLength(result.mode, len);
	assert(bitLen != LENGTH_OVERFLOW);
	result.numChars = (int)len;
	if (bitLen > 0)
		memset(buf, 0, ((size_t)bitLen + 7) / 8 * sizeof(buf[0]));
	result.bitLength = 0;
	
	unsigned int accumData = 0;
	int accumCount = 0;
	for (; *digits != '\0'; digits++) {
		char c = *digits;
		assert('0' <= c && c <= '9');
		accumData = accumData * 10 + (unsigned int)(c - '0');
		accumCount++;
		if (accumCount == 3) {
			appendBitsToBuffer(accumData, 10, buf, &result.bitLength);
			accumData = 0;
			accumCount = 0;
		}
	}
	if (accumCount > 0)  // 1 or 2 digits remaining
		appendBitsToBuffer(accumData, accumCount * 3 + 1, buf, &result.bitLength);
	assert(result.bitLength == bitLen);
	result.data = buf;
	return result;
}


// Public function - see documentation comment in header file.
struct qrcodegen_Segment qrcodegen_makeAlphanumeric(const char *text, uint8_t buf[]) {
	assert(text != NULL);
	struct qrcodegen_Segment result;
	size_t len = strlen(text);
	result.mode = qrcodegen_Mode_ALPHANUMERIC;
	int bitLen = calcSegmentBitLength(result.mode, len);
	assert(bitLen != LENGTH_OVERFLOW);
	result.numChars = (int)len;
	if (bitLen > 0)
		memset(buf, 0, ((size_t)bitLen + 7) / 8 * sizeof(buf[0]));
	result.bitLength = 0;
	
	unsigned int accumData = 0;
	int accumCount = 0;
	for (; *text != '\0'; text++) {
		const char *temp = strchr(ALPHANUMERIC_CHARSET, *text);
		assert(temp != NULL);
		accumData = accumData * 45 + (unsigned int)(temp - ALPHANUMERIC_CHARSET);
		accumCount++;
		if (accumCount == 2) {
			appendBitsToBuffer(accumData, 11, buf, &result.bitLength);
			accumData = 0;
			accumCount = 0;
		}
	}
	if (accumCount > 0)  // 1 character remaining
		appendBitsToBuffer(accumData, 6, buf, &result.bitLength);
	assert(result.bitLength == bitLen);
	result.data = buf;
	return result;
}


// Public function - see documentation comment in header file.
struct qrcodegen_Segment qrcodegen_makeEci(long assignVal, uint8_t buf[]) {
	struct qrcodegen_Segment result;
	result.mode = qrcodegen_Mode_ECI;
	result.numChars = 0;
	result.bitLength = 0;
	if (assignVal < 0)
		assert(false);
	else if (assignVal < (1 << 7)) {
		memset(buf, 0, 1 * sizeof(buf[0]));
		appendBitsToBuffer((unsigned int)assignVal, 8, buf, &result.bitLength);
	} else if (assignVal < (1 << 14)) {
		memset(buf, 0, 2 * sizeof(buf[0]));
		appendBitsToBuffer(2, 2, buf, &result.bitLength);
		appendBitsToBuffer((unsigned int)assignVal, 14, buf, &result.bitLength);
	} else if (assignVal < 1000000L) {
		memset(buf, 0, 3 * sizeof(buf[0]));
		appendBitsToBuffer(6, 3, buf, &result.bitLength);
		appendBitsToBuffer((unsigned int)(assignVal >> 10), 11, buf, &result.bitLength);
		appendBitsToBuffer((unsigned int)(assignVal & 0x3FF), 10, buf, &result.bitLength);
	} else
		assert(false);
	result.data = buf;
	return result;
}


// Calculates the number of bits needed to encode the given segments at the given version.
// Returns a non-negative number if successful. Otherwise returns LENGTH_OVERFLOW if a segment
// has too many characters to fit its length field, or the total bits exceeds INT16_MAX.
testable int getTotalBits(const struct qrcodegen_Segment segs[], size_t len, int version) {
	assert(segs != NULL || len == 0);
	long result = 0;
	for (size_t i = 0; i < len; i++) {
		int numChars  = segs[i].numChars;
		int bitLength = segs[i].bitLength;
		assert(0 <= numChars  && numChars  <= INT16_MAX);
		assert(0 <= bitLength && bitLength <= INT16_MAX);
		int ccbits = numCharCountBits(segs[i].mode, version);
		assert(0 <= ccbits && ccbits <= 16);
		if (numChars >= (1L << ccbits))
			return LENGTH_OVERFLOW;  // The segment's length doesn't fit the field's bit width
		result += 4L + ccbits + bitLength;
		if (result > INT16_MAX)
			return LENGTH_OVERFLOW;  // The sum might overflow an int type
	}
	assert(0 <= result && result <= INT16_MAX);
	return (int)result;
}


// Returns the bit width of the character count field for a segment in the given mode
// in a QR Code at the given version number. The result is in the range [0, 16].
static int numCharCountBits(enum qrcodegen_Mode mode, int version) {
	assert(qrcodegen_VERSION_MIN <= version && version <= qrcodegen_VERSION_MAX);
	int i = (version + 7) / 17;
	switch (mode) {
		case qrcodegen_Mode_NUMERIC     : { static const int temp[] = {10, 12, 14}; return temp[i]; }
		case qrcodegen_Mode_ALPHANUMERIC: { static const int temp[] = { 9, 11, 13}; return temp[i]; }
		case qrcodegen_Mode_BYTE        : { static const int temp[] = { 8, 16, 16}; return temp[i]; }
		case qrcodegen_Mode_KANJI       : { static const int temp[] = { 8, 10, 12}; return temp[i]; }
		case qrcodegen_Mode_ECI         : return 0;
		default:  assert(false);  return -1;  // Dummy value
	}
}


#undef LENGTH_OVERFLOW
