# znet-jolt

Rigid body state on the wire, for [Jolt](https://github.com/jrouwe/JoltPhysics).

Header-only. Everything lives in `znet::ext::jolt`. Builds on
[`znet-quantize`](../quantize/) and [`znet-bitpack`](../bitpack/).

**Requires C++17**, which is Jolt's own floor. On a C++14 tree this
extension skips itself and says so.

```cmake
target_link_libraries(my_game PRIVATE znet-jolt)   # or znet::jolt
```

```cpp
#include "znet/ext/jolt/jolt.h"
```

## What it saves

| | Raw | Here |
| --- | --- | --- |
| position (`JPH::Vec3`) | 12 B | 48 bits |
| orientation (`JPH::Quat`) | 16 B | **32 bits** |
| linear velocity | 12 B | 36 bits |
| **total** | **40 B** | **116 bits (15 B)** |

The orientation is the win. A unit quaternion carries three degrees of freedom,
so the fourth component is redundant; the smallest-three encoding drops the
largest one and recovers it from the other three. Worst-case error is 0.115°,
and the identity is exact, so a body at rest does not jitter.

## Usage

```cpp
BitWriter bits(*buffer);
znet::ext::jolt::WriteTransform(bits, body.GetPosition(), body.GetRotation(), quantization);
znet::ext::jolt::WriteLinearVelocity(bits, body.GetLinearVelocity(), quantization);
```

```cpp
BitReader bits(*buffer);
JPH::Vec3 position;
JPH::Quat orientation;
znet::ext::jolt::ReadTransform(bits, position, orientation, quantization);
const JPH::Vec3 velocity = znet::ext::jolt::ReadLinearVelocity(bits, quantization);
```

`BodyQuantization` holds the world bounds and field widths. **Both ends must
agree on it**; it is not sent, because sending it every tick would cost more
than the fields it describes. The defaults suit a world about two thousand
units across, landing positions within 3 cm.

The functions take Jolt's **math types**, not its body types, so this pulls
in nothing beyond Jolt's math headers. Feed them straight from your bodies.

## Everything decoded is usable

Jolt's maths assumes its quaternions are unit length, and a denormalised one
silently skews every transform it touches. Whatever bits arrive, `ReadOrientation`
returns a unit quaternion, asserted over 20000 random words. Positions outside
the configured world clamp to its bounds rather than wrapping.

## Why it shares an implementation

The compression arithmetic is [`znet-quantize`](../quantize/)'s, the same code
`znet-glm` and the other engine adapters use. That matters when a server built
against one and a client built against another talk to each other: identical
bytes have to mean identical transforms, which two independent copies of the
same rounding would not guarantee.

## Building

Uses an installed Jolt if there is one, otherwise fetches 5.2.0 with its samples
and tests off. The fetched copy is marked `SYSTEM`.

```
-DZNET_EXT_JOLT=OFF          just this extension off
-DZNET_EXT_ALLOW_FETCH=OFF  skip rather than download
```

## Tests

`ctest -R ext-jolt-tests`. 5000 random transforms round-tripped against
uniformly sampled rotations, the identity checked for exactness, 20000 random
words checked for unit length, and clamping at the world bounds.

Verified at C++17 and above, on GCC and Clang, against Jolt 5.2.
