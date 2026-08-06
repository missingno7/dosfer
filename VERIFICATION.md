# Verification summary

Build ID: `dosfer32-stream-r24-legacy-hotpath`

Completed before packaging:

- strict GCC host build with warnings treated as errors
- strict Clang host build with warnings treated as errors
- protocol and whitening self-test
- centered raster equivalence tests
- PLANE3/PLANE4 affine codeword and raster tests
- cooperative V40-L encoder comparison across 128 randomized cases
- representative end-to-end simulated VGA tests
- 840-case PLANE/window/EOF boundary matrix with visible-raster integrity checks
- source diff whitespace validation
- aggregate host syntax check for the DOS translation units using DOS API stubs

Not completed in the packaging environment:

- Open Watcom/DOS4GW compilation
- DOSBox-X fixed-3000-cycle performance benchmark
- physical VGA/CRT optical test

The old pre-fix `dos_sender32/DOSFER32.EXE` was deliberately removed. Build the
new executable with `dos_sender32/build32.bat` and Open Watcom v2.
