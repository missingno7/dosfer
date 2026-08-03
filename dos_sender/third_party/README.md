# Third-party source

`qrcodegen.c` and `qrcodegen.h` began from the C implementation of Nayuki's QR
Code generator library. DOSfer adds a documented DOS single-thread fast path:
GF(256) tables, cached Reed-Solomon data, direct byte-segment packing, cached
module placement, and a precomputed fixed-mask bitmap. QR output remains
standards-compliant and the public API is unchanged. The source files
carry the full MIT license notice. Upstream:
https://github.com/nayuki/QR-Code-generator
