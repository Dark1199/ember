# MSB (map studio) files

Source: public community documentation of the PC release's map files, written up here in our own
words (CLEANROOM.md rule 3). **Not yet verified against real files by this project.** The parser
checks what it can and the startup log prints one summary line per map (`msb: ...`), so wrong field
positions show up as implausible numbers on the device before anything depends on them.

An MSB describes one map block: which models it uses, and where each model is placed. Files live at
`/map/MapStudio/mAA_BB_00_00.msb` and are not DCX wrapped in this release (a wrapped one is
decoded by `Vfs::ReadAllDecoded` before parsing). All values are little-endian.

## Container

The file is a chain of parameter tables, starting at offset 0:

| offset | type | meaning |
|---|---|---|
| 0x00 | i32 | version, 0 |
| 0x04 | i32 | N, number of offsets that follow including the last one (entries = N - 1) |
| 0x08 | i32 | absolute offset of this table's name (ASCII, NUL terminated) |
| 0x0C | i32[N-1] | absolute offsets of the entries |
| after | i32 | absolute offset of the next table, or 0 / the file size after the last one |

Table order in practice: `MODEL_PARAM_ST`, `EVENT_PARAM_ST`, `POINT_PARAM_ST`, `PARTS_PARAM_ST`.
The parser follows the chain by the stored offsets, not by assuming the order.

Strings inside entries are Shift-JIS; names seen so far are plain ASCII. The parser keeps raw bytes.

## Model entry (0x20 bytes of header)

| offset | type | meaning |
|---|---|---|
| 0x00 | i32 | name offset, relative to the entry start |
| 0x04 | i32 | type: 0 map piece, 1 object, 2 enemy, 4 player, 5 collision, 6 navmesh |
| 0x08 | i32 | id (index among models of the same type) |
| 0x0C | i32 | sib path offset, relative to the entry start |
| 0x10 | i32 | instance count (how many parts use this model) |
| 0x14 | 12 bytes | zero padding |

## Part entry (0x64 bytes of common header)

| offset | type | meaning |
|---|---|---|
| 0x00 | i32 | name offset, relative to the entry start |
| 0x04 | i32 | type: 0 map piece, 1 object, 2 enemy, 4 player, 5 collision, 8 navmesh, 9 dummy object, 10 dummy enemy, 11 connect collision |
| 0x08 | i32 | id |
| 0x0C | i32 | model index: position in the model table (0-based) |
| 0x10 | i32 | sib path offset, relative to the entry start |
| 0x14 | f32[3] | position |
| 0x20 | f32[3] | rotation, degrees (Euler) |
| 0x2C | f32[3] | scale |
| 0x38 | u32[4] | draw group bit masks |
| 0x48 | u32[4] | display group bit masks |
| 0x58 | i32 | entity data offset, relative to the entry start |
| 0x5C | i32 | type specific data offset, relative to the entry start |
| 0x60 | i32 | zero |

Type specific data (map piece, object, enemy, collision, ...) and the event and point tables are
not decoded yet; the parser only counts their entries.

## Using it

A part of type map piece names its model through `model index`; the model's name is the model
file's base name. Resolving that to a file in the archives is a separate step and is not assumed
here.
