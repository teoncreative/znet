# znet-bullet

Rigid body state on the wire, for [Bullet](https://github.com/bulletphysics/bullet3).

Header-only. Everything lives in `znet::ext::bullet`. Builds on
[`znet-quantize`](../quantize/) and [`znet-bitpack`](../bitpack/).

```cmake
target_link_libraries(my_game PRIVATE znet-bullet)   # or znet::bullet
```

```cpp
#include "znet/ext/bullet/bullet.h"
```

## What it saves

| | Raw | Here |
| --- | --- | --- |
| position (`btVector3`) | 12 B | 48 bits |
| orientation (`btQuaternion`) | 16 B | **32 bits** |
| linear velocity | 12 B | 36 bits |
| **total** | **40 B** | **116 bits (15 B)** |

The orientation is the win. A unit quaternion carries three degrees of freedom,
so the fourth component is redundant; the smallest-three encoding drops the
largest one and recovers it from the other three. Worst-case error is 0.115°,
and the identity is exact, so a body at rest does not jitter.

## Usage

```cpp
BitWriter bits(*buffer);
znet::ext::bullet::WriteTransform(bits, transform.getOrigin(), transform.getRotation(), quantization);
znet::ext::bullet::WriteLinearVelocity(bits, body->getLinearVelocity(), quantization);
```

```cpp
BitReader bits(*buffer);
btVector3 position;
btQuaternion orientation;
znet::ext::bullet::ReadTransform(bits, position, orientation, quantization);
const btVector3 velocity = znet::ext::bullet::ReadLinearVelocity(bits, quantization);
```

`BodyQuantization` holds the world bounds and field widths. **Both ends must
agree on it**; it is not sent, because sending it every tick would cost more
than the fields it describes. The defaults suit a world about two thousand
units across, landing positions within 3 cm.

The functions take Bullet's **math types**, not its body types, so this pulls
in nothing beyond LinearMath. Feed them straight from your bodies.

## Everything decoded is usable

Bullet's maths assumes its quaternions are unit length, and a denormalised one
silently skews every transform it touches. Whatever bits arrive, `ReadOrientation`
returns a unit quaternion — asserted over 20000 random words. Positions outside
the configured world clamp to its bounds rather than wrapping.

## Why it shares an implementation

The compression arithmetic is [`znet-quantize`](../quantize/)'s, the same code
`znet-glm` and the other engine adapters use. That matters when a server built
against one and a client built against another talk to each other: identical
bytes have to mean identical transforms, which two independent copies of the
same rounding would not guarantee.

## Building

Uses an installed Bullet if there is one, otherwise fetches 3.25 with the demos,
extras and tests off — they are the bulk of its 136 MB and none of the use.

Two wrinkles worth knowing, both handled here. Bullet 3.25 declares
`cmake_minimum_required(VERSION 2.4.3)` and CMake 4 removed compatibility below
3.5, so the fetch sets `CMAKE_POLICY_VERSION_MINIMUM` for the duration and
restores it afterwards rather than loosening policy for znet's own targets. And
Bullet's targets predate usage requirements and carry no interface include
directory, so this extension adds it explicitly.

```
-DZNET_EXT_BULLET=OFF          just this extension off
-DZNET_EXT_ALLOW_FETCH=OFF  skip rather than download
```

## Tests

`ctest -R ext-bullet-tests`. 5000 random transforms round-tripped against
uniformly sampled rotations, the identity checked for exactness, 20000 random
words checked for unit length, and clamping at the world bounds.

Verified at C++14 and above, on GCC and Clang, against Bullet 3.25.
