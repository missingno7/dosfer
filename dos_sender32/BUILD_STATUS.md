# Build status

Source build ID:

```text
dosfer32-stream-r24-legacy-hotpath
```

Completed in this package:

- strict GCC host compilation with warnings treated as errors
- all protocol, raster, affine and incremental-encoder tests
- end-to-end simulated VGA pipeline tests
- 840-case PLANE/window/EOF boundary matrix

Not completed in the packaging environment:

- Open Watcom/DOS4GW compilation
- DOSBox-X fixed-3000-cycle measurement
- physical VGA/CRT optical test

No old `DOSFER32.EXE` is included as though it were the fixed build. Run
`build32.bat` with Open Watcom to create `build\DOSFER32.EXE`.
