# PLIV version 23 reconstruction notes

These notes describe confirmed behavior from `pactLoadAction`,
`__pactLoadAniBone`, and `_pactLoadAniUnflowedTexture` in the original ARM
`libgame.so`. Names for unknown fields remain deliberately conservative.

## Container and motion table

The file begins with `PLM`, followed by version 23. Byte 4 is the motion count
and byte 5 is the auxiliary-object count. The version-23 motion table begins at
offset 26 and contains one seven-byte record per motion.

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | Feature flags, little-endian |
| 2 | 1 | Attachment-track count |
| 3 | 1 | Frames per second |
| 4 | 1 | Sample count |
| 5 | 2 | Frame count, little-endian |

Confirmed feature bits are `0x0100` for bone animation, `0x0200` for flowed
texture animation, `0x0400` for unflowed texture/UV animation, and `0x1000`
for vertex animation. The original 26 Korean PLIV files contain 58 motions:
13 files use bones, 10 use unflowed UV tracks, and 9 use vertex tracks. No
shipped file uses the flowed-UV bit.

## Unflowed texture section

This section follows any bone or flowed-texture section. Its first four bytes
are ignored by the original loader, followed by a 16-bit total key count. Each
motion carrying bit `0x0400` contains a track count. Each track then stores a
texture-unit byte, a `frameCount / 8 + 1` key mask, and one little-endian
16-bit U/V pair for every set bit. Frames without a key reuse the preceding
key. Some files finish with an optional `****` footer.

The Windows decoder completely consumes all ten language copies that have no
preceding bone section. `gun_fire.pliv` is the reference case: 11 frames at
30 fps, ten UV keys, one track, and no trailing bytes.

## Bone section and tracks

The bone section starts with a seven-byte capacity header: bone count, a raw
16-bit frame-table capacity, 16-bit rotation-cache capacity, and 16-bit
position-cache capacity. These cache capacities are not the number of keys
physically stored in the stream. They are followed by six signed 32-bit
position bounds: minimum X/Y/Z and maximum X/Y/Z.

Version 23 stores a 17-byte descriptor for each bone: self, parent, next-sibling,
and first-child identifiers; a 16-bit vertex count; four signed 16-bit
bind-rotation components; and three quantized bind-position bytes. The vertex
counts are consecutive PMOD ranges. They sum exactly to the associated PMOD
vertex count for the recovered enemies (`80 + 185 = 265` for `zako01`,
`83 + 248 = 331` for `zako02`, and `24 + 122 + 183 = 329` for `zako03`).
Version 24 introduced an additional leading hierarchy byte and therefore uses
an 18-byte descriptor; keeping these layouts separate matches the original
version branches in `__pactLoadAniBone`.

Each bone-enabled motion then stores a rotation track and position track for
each bone. A zero leading byte selects a constant track. Constant rotations
store four signed 16-bit components; constant positions store three quantized
bytes. A nonzero leading byte is also the first byte of a
`frameCount / 8 + 1` key mask. Every tagged rotation frame stores four signed
16-bit components, while every tagged position frame stores three quantized
bytes. Untagged frames reuse index zero until the first tagged frame and then
reuse the preceding key, matching the zero-initialized lookup arrays in the
original loader. Quaternions are normalized after expansion. Position bytes
expand through `minimum + byte * ((maximum - minimum) >> 8)`.

The native decoder can currently walk the per-frame rotation/position tracks
for 18 language copies (nine original bone files). That count is a mechanical
cursor result, not proof that every combined section ends at the correct byte.
Several multi-bone player and combined-section files still use unresolved
stream-layout variants; their headers and bind tables decode, but the decoder
deliberately reports a boundary error instead of accepting a guessed layout.

## Vertex animation section

Runtime capture of `_pactLoadAniVertex` confirmed that its input pointer is the
start of the four-byte `****` marker. The loader skips those four bytes and
then reads a 16-bit remap-index count, a 16-bit packed-entry count with four
bytes per entry, and a 16-bit packed-position count with three bytes per
entry. The four-byte entries are 8.8 U/V coordinates. A reserved 16-bit value
and a 16-bit material-record count precede 13-byte version-23 material
records.

Each vertex-enabled motion then has a 37-byte descriptor: three
offset/count pairs selecting the remap, U/V, and packed-position arrays; one
attachment-track-count byte; and six signed 16.16 bounds. Packed XYZ bytes
expand with the same native equation used by the ARM loader:
`minimum + byte * ((maximum - minimum) >> 8)`. The motion header supplies the
frame count and playback rate. Index ranges, U/V ranges, packed-position
ranges, and every remap reference are validated before frames are exposed.

The decoder now expands all 18 language copies of the nine vertex-animation
files. The recovered `ENEMY_zako01` section begins at file offset 221 and
consumes the exact 2,067-byte file: 72 indices, 48 U/V entries, and 480 packed
positions representing 48 vertices across ten frames at 30 fps.
`ENEMY_bMissile` similarly contains 180 indices and 61 vertices across ten
30-fps frames. The Windows renderer appends these animated draw ranges after
the static PMOD range, matching the original PMOD attribute start offsets,
and linearly prepares adjacent source frames instead of generating synthetic
motion.
