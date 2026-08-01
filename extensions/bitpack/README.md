# znet-bitpack

Sub-byte fields in an ordinary `Buffer`. A bool costs one bit instead of eight;
a value known to be under 1000 costs ten bits instead of sixteen.

Header-only, **no third-party dependency**. Everything lives in `znet::ext`.

```cmake
target_link_libraries(my_game PRIVATE znet-bitpack)   # or znet::bitpack
```

```cpp
#include "znet/ext/bitpack/bitpack.h"
```

## Why

Every core `Buffer` field rounds up to a whole byte. A tick of game state is
mostly small bounded fields, and that rounding is routinely a third of the
packet:

```cpp
// 13 bytes with the core Buffer
buffer->WriteBool(firing);            // 1 byte  for 1 bit
buffer->WriteInt<uint16_t>(ammo);     // 2 bytes for a 0-200 value
buffer->WriteInt<int32_t>(health);    // 4 bytes for a -50..100 value
buffer->WriteFloat(yaw);              // 4 bytes for an angle
```

```cpp
// 4 bytes with a BitWriter
BitWriter bits(*buffer);
bits.WriteBool(firing);                          //  1 bit
bits.WriteUIntRanged(ammo, 0, 200);              //  8 bits
bits.WriteIntRanged(health, -50, 100);           //  8 bits
bits.WriteFloatRanged(yaw, -3.15f, 3.15f, 12);   // 12 bits
```

## Usage

Open a writer over the buffer, write fields, let it go out of scope.

```cpp
buffer->WriteInt<uint32_t>(entity_id);      // ordinary buffer field
{
  BitWriter bits(*buffer);
  bits.WriteBool(packet->firing);
  bits.WriteUIntRanged(packet->ammo, 0, 200);
  bits.WriteFloatRanged(packet->yaw, -3.15f, 3.15f, 12);
}                                            // flushes on scope exit
buffer->WriteString(packet->name);           // ordinary buffer field again
```

```cpp
const uint32_t entity_id = buffer->ReadInt<uint32_t>();
{
  BitReader bits(*buffer);
  packet->firing = bits.ReadBool();
  packet->ammo   = bits.ReadUIntRanged(0, 200);
  packet->yaw    = bits.ReadFloatRanged(-3.15f, 3.15f, 12);
  if (!bits.ok()) return nullptr;            // ran off the end
}
packet->name = buffer->ReadString();
```

The destructor flushes. That is the reason to scope it rather than keep one
around: a bit packer that forgets its final partial byte loses the last few
fields, silently, and only for some payload lengths. Call `Flush()` explicitly
if you need the byte count before the writer dies.

After a flush the buffer is byte-aligned again, so bit fields and ordinary
`Buffer` fields interleave freely within one packet.

## Fields

| Writer | Reader | Cost |
| --- | --- | --- |
| `WriteBool` | `ReadBool` | 1 bit |
| `WriteBits(v, n)` | `ReadBits(n)` | n bits, n ≤ 32 |
| `WriteBits64(v, n)` | `ReadBits64(n)` | n bits, n ≤ 64 |
| `WriteUIntRanged(v, min, max)` | `ReadUIntRanged(min, max)` | `BitsForRange(min, max)` |
| `WriteIntRanged(v, min, max)` | `ReadIntRanged(min, max)` | `BitsForSignedRange(min, max)` |
| `WriteFloatRanged(v, min, max, n)` | `ReadFloatRanged(min, max, n)` | n bits |
| `WriteVarUInt(v)` | `ReadVarUInt()` | 5 bits per 4 data bits |

Prefer the **ranged** calls over raw `WriteBits`. They compute their own width
from the bounds, so there is no hand-counted bit count to get subtly wrong, and
`BitsForRange` folds at compile time when the bounds are literals. A range with
one possible value costs zero bits.

`WriteVarUInt` is for counts with no natural upper bound: values under 16 cost
5 bits, under 256 cost 10, worst case 80. If a bound exists at all,
`WriteUIntRanged` is always smaller.

## Safety

Nothing in the stream is self-describing, and a reader that disagrees with the
writer about a field's width desynchronises from there on. That is why the
ranged calls take bounds rather than widths wherever they can.

Given that, the decoders are built so a malformed packet cannot become a memory
bug:

- **Ranged reads clamp to their bounds.** A field wide enough for `[0, 5]` can
  physically express 6 and 7, and a hostile sender can put them there. The read
  returns 5. A decoded value may be wrong, but it is never out of range, so it
  is safe to use directly as an array index or a loop count.
- **`ReadVarUInt` is bounded.** A stream of nothing but continuation groups
  stops after 16, and a truncated one stops when the bytes run out.
- **Reads past the end return zero** and set `ok()` false. They also record
  `ReadOutOfBounds` on the buffer, so a caller already checking
  `GetAndClearLastError()` once per packet catches it without a second check.

## Precision ceiling on ranged floats

Worst-case error is `(max - min) / (2 * (2^bits - 1))`, but only down to a
point. Past roughly 21 bits the step becomes smaller than the gap between
adjacent float32 values, so the result cannot get closer to the input and the
extra bits are spent for nothing. A field needing more precision than that
needs a narrower range or a double, not a wider field.

## Bit order

Bits fill least-significant-first within each byte; bytes reach the buffer in
order. Writing the 3-bit value `0b101` and then the 2-bit value `0b11` produces
one byte `0b00011101`, the first field in bits 0-2, the second in bits 3-4,
unused high bits zero.

This is the convention `Buffer::WriteBitset` already uses. It is unrelated to
the buffer's endianness setting, which orders bytes within one multi-byte
number; a bit stream reaches the buffer as a byte stream, so the two never
interact.

## Tests

`ctest -R ext-bitpack-tests`. Beyond the per-feature cases, the suite runs 2000
random sequences of up to 200 fields at random widths, because a bit packer's
bugs live at scratch-flush and byte boundaries and only appear for particular
width sequences. It also decodes 2000 random byte strings to confirm every
ranged read stays inside its bounds.

Verified at C++14, 17, 20 and 23, on GCC and Clang.
