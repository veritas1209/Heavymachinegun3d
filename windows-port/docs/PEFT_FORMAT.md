# PEFT version 23 reconstruction notes

The shipped effect files begin with `PEM` and version 23. `pactLoadEffectEx`
confirms a 25-byte common header followed by one five-byte record per motion.

| Motion offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | Feature flags, little-endian |
| 2 | 1 | Frames per second |
| 3 | 2 | Frame count, little-endian |

All shipped files contain one motion. The payload starts immediately after the
motion table and has the following version-23 layout. Counts are little-endian.

| Order | Layout | Confirmed handling |
|---:|---|---|
| 1 | 4-byte common prefix | Skipped by `_pactLoadAniVertex` |
| 2 | `u16` count + `count * u16` | Remap indices |
| 3 | `u16` count + `count * u32` | Packed entries |
| 4 | `u16` count + `count * 3 bytes` | Packed triples |
| 5 | `u16` byte count + bytes | Auxiliary byte block |
| 6 | `u16` count + `count * 13 bytes` | Six `u16` fields and one track-count byte per base record |
| 7 | `u16` | Mask-section footer |
| 8 | 35 bytes | Repeated capacities and six signed 32-bit position bounds |
| 9 | `u16` byte count + bytes | Primary frame masks |
| 10 | `u16` byte count + bytes | Optional secondary frame masks |

The 35-byte verification block contains `u32` remap capacity, `u32` packed-entry
capacity, `u16` packed-triple capacity, `u8` base-record count, and minimum/maximum
XYZ bounds as six signed 32-bit values. The repeated capacities must exactly match
the earlier decoded tables.

Each frame mask occupies `frameCount / 8 + 1` bytes per participating base record.
The primary stream covers every base record. The secondary stream is absent or
covers a whole-number subset of records. Boss laser is the shipped boundary case:
three primary masks and two secondary masks. This rule fully consumes all 22
English/Korean PEFT copies with no trailing bytes.

## Runtime use

The Windows runtime includes the original descriptors for hit, enemy destruction,
missile explosion, shield field, thunder fire, and boss laser. Startup decodes each
file and refuses a partial descriptor. Their original FPS and frame counts now set
effect lifetimes and sprite-atlas progression; decoded duration examples are 200 ms
for hit, 734 ms for enemy destruction, 867 ms for missile explosion, 367 ms for
shield, 300 ms for thunder, and 1700 ms for boss laser.

Mechanical decoding does not yet assign engine-level meanings to every packed
entry/triple, base-record field, or mask bit. Native particle geometry,
interpolation, blend selection, and emitter semantics therefore remain a renderer
reconstruction task even though byte boundaries and timing are now exact.
