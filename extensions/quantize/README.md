# znet-quantize

The quantisation arithmetic, on plain floats and integers.

Header-only, **no dependency at all**, not even a `Buffer`. Everything lives in
`znet::ext::quant`.

```cpp
#include "znet/ext/quantize/quantize.h"
```

## Why it is separate

`znet-glm`, `znet-bitpack`, `znet-box2d`, `znet-bullet` and `znet-jolt` all
compress the same kinds of value. If each carried its own copy of the
arithmetic, two copies differing by a single rounding would produce a stream
that mostly works and occasionally does not, and a server built against one
and a client built against another would disagree about what identical bytes
mean. There is one copy, and it is here.

You will not usually include this directly. Reach for the extension that speaks
your types; it delegates here.

## What it offers

| | |
| --- | --- |
| `QuantizeToLevels` / `DequantizeFromLevels` | fixed point over `[min, max]` onto `[0, levels]` |
| `QuantizeToInt<UInt>` / `DequantizeFromInt<UInt>` | the same, onto the full range of an integer type |
| `LevelsForBits(n)` | `2^n - 1`, saturating at 64 bits |
| `PackHalf` / `UnpackHalf` | IEEE 754 binary16 |
| `PackQuatSmallestThree` / `UnpackQuatSmallestThree` | unit quaternion in 32 bits |
| `PackAngle` / `UnpackAngle` | planar orientation, wrapping at the seam |
| `PackDirectionOct` / `UnpackDirectionOct` | unit vector, octahedral |
| `OctEncode` / `OctDecode` | the octahedral mapping on its own |

Measured worst cases, asserted in the tests: smallest-three **0.115°** with the
identity exact, octahedral **0.0037°** at 16 bits per axis and **0.94°** at 8.

## Decisions worth knowing

**Fixed point runs in double.** A 32-bit code has more distinct values than a
float has mantissa bits, so computing the scale in float would leave whole
swathes of codes unreachable.

**Smallest-three uses 1023 levels, not 1024.** An even count puts zero halfway
between two codes, which would make the identity quaternion decode as a 0.137°
rotation and a resting object jitter. An odd count puts a code exactly on zero.
The price is one unused code out of 1024.

**Angles wrap.** `PackAngle` treats the range as a circle, so +π and −π get the
same code and a body spinning through the wrap does not jump a step.

**Decoders never trust their input.** Any 32-bit word decodes to a finite unit
quaternion; any word decodes to a unit direction. A decoded value can go
straight into a matrix.

## Tests

`ctest -R ext-quantize-tests`. Notably: **all 65536 binary16 bit patterns** must
survive decode-then-encode, and `PackHalf` is checked against an independent
reference written via `frexp`/`ldexp` over 400k random floats. That pair caught
a real bug: an off-by-one that made every subnormal decode at half its value,
present in the decode direction only, which the reference comparison alone would
have missed.

Verified at C++14, 17, 20 and 23, on GCC and Clang.
