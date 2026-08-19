# SPL3 / QPDL v3 stream format (HP Laser 1003-1008)

Reverse-engineered from the printer's behaviour, cross-checked against SpliX's `qpdl.cpp`
and HP's `rastertospl` output (and its disassembly). This is enough to encode and decode
these streams without HP's binary. `spl3dump.py` implements the parser below.

A job is: **PJL header** (ASCII) then a **QPDL body** (binary) then a **UEL terminator**.

```
<UEL> @PJL ... @PJL ENTER LANGUAGE = QPDL \r\n   <-- PJL header
<QPDL binary: page records + band records>       <-- body
<UEL>                                             <-- terminator
```

`UEL` = `1B 25 2D 31 32 33 34 35 58` (`\x1B%-12345X`).

## Page header record (17 bytes, 0x11)

Emitted once per page. First byte is a 0x00 signature.

| offset | bytes | field | notes |
|---|---|---|---|
| 0x00 | 1 | signature | always `00` |
| 0x01 | 1 | Y resolution / 100 | `06` = 600 dpi |
| 0x02 | 2 | copies | big-endian |
| 0x04 | 1 | paper type | `02` = A4, `00` = Letter, ... |
| 0x05 | 2 | **printable width** | big-endian, **on a 300-dpi grid** |
| 0x07 | 2 | **printable height** | big-endian, **on a 300-dpi grid** |
| 0x09 | 1 | paper source | |
| 0x0A | 1 | (vendor) | |
| 0x0B | 1 | duplex | |
| 0x0C | 1 | tumble | |
| 0x0D | 1 | (vendor) | |
| 0x0E | 1 | **QPDL version** | `03` for this family |
| 0x0F | 1 | (vendor) | |
| 0x10 | 1 | X resolution / 100 | `06` = 600 dpi |

**The 300-dpi grid is the key quirk.** Width/height are `ceil(points * 300 / 72)`
regardless of the actual raster resolution. For A4 that is `2480 x 3507`. HP's binary
computes exactly this (`fmul 300.0 / fdiv 72.0` in `rastertospl`). Emitting the real
600-dpi size (`4960 x 6912`) makes the firmware think the page is ~16x23 inches and it
spits a striped sheet per band. This one field was the whole bug.

## Band record

The page raster is split into horizontal bands (typically 128 rows tall). Each band:

| offset | bytes | field |
|---|---|---|
| 0x00 | 1 | signature, always `0C` |
| 0x01 | 1 | band number |
| 0x02 | 2 | band width in pixels (big-endian) |
| 0x04 | 2 | band height in pixels (big-endian) |
| 0x06 | 1 | compression algorithm |
| 0x07 | 4 | payload size (big-endian) |
| 0x0B | payload | data signature `EF CD AB 09` + compressed plane data (+ 4-byte checksum for v>0) |

Band width for this family is fixed by a per-paper table (`SpecialBandWidth`), e.g. A4 at
600 dpi = 4864 px (608 bytes). Compression `0x11` is a lossless LZ back-reference codec
(SpliX `algo0x11`): a small table of common back-reference offsets, then literal runs and
back-references. Because it is lossless, any valid `0x11` stream decodes to the same
pixels, which is why two encoders producing different bytes both print correctly.

Other compression ids: `0x0D`/`0x0E` (no data signature), `0x13`, `0x15` (JBIG). This
family uses `0x11`.

## End of job

After the last band: a short trailer (e.g. `01 00 01 09`) then the UEL terminator.

## Worked example (`spl3dump.py` on an A4 page)

```
PAGE 1
  resolution   : 600 x 600 dpi
  paper        : A4
  page size    : 2480 x 3507  (300-dpi grid -> 210.0 x 296.9 mm)
  QPDL version : 3
  BAND  0: 4864 x 128  comp=0x11 (LZ back-reference)  payload=588B  sig=ef cd ab 09
  ...
```
