# Third-party source

`qrcodegen.c` and `qrcodegen.h` began from the C implementation of Nayuki's QR
Code generator library. DOSfer keeps the canonical implementation as its
correctness oracle and adds a documented, single-threaded DOS V40-L fast path:

- GF(256) and degree-30 Reed-Solomon lookup tables;
- fixed V40-L frame packing;
- the proven two-input-byte 16-bit RS recurrence;
- block-major ECC output for the fused streaming renderer;
- a compact linear module-placement cache whose allocation is transferred to
  the VGA backend and converted in place;
- affine C2 codeword derivation.

QR output remains standards-compliant. The source files carry the full MIT
license notice. Upstream:
https://github.com/nayuki/QR-Code-generator
