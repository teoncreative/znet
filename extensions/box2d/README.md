# znet-box2d

Planar rigid body state on the wire, for [Box2D](https://box2d.org) v3.

Header-only. Everything lives in `znet::ext`. Builds on
[`znet-quantize`](../quantize/) and [`znet-bitpack`](../bitpack/).

```cmake
target_link_libraries(my_game PRIVATE znet-box2d)   # or znet::box2d
```

```cpp
#include "znet/ext/box2d/box2d.h"
```

## Why 2D is cheap

A planar body's orientation is a **single angle**, not a quaternion, so it costs
10 bits where a 3D body spends 32. A full transform is 42 bits:

| | Raw | Here |
| --- | --- | --- |
| position (`b2Vec2`) | 8 B | 32 bits |
| rotation (`b2Rot`) | 8 B | 10 bits |
| **transform** | **16 B** | **42 bits (6 B)** |
| + linear and angular velocity | 28 B total | **76 bits (10 B)** |

`b2Rot` is a cosine and a sine — two floats for one degree of freedom. Sending
the angle instead costs a third as much and cannot arrive denormalised, because
*any* code decodes to a unit `b2Rot`.

## Usage

```cpp
BitWriter bits(*buffer);
znet::ext::WriteTransform(bits, b2Body_GetTransform(body), quantization);
znet::ext::WriteLinearVelocity(bits, b2Body_GetLinearVelocity(body), quantization);
znet::ext::WriteAngularVelocity(bits, b2Body_GetAngularVelocity(body), quantization);
```

```cpp
BitReader bits(*buffer);
const b2Transform transform = znet::ext::ReadTransform(bits, quantization);
const b2Vec2 linear = znet::ext::ReadLinearVelocity(bits, quantization);
const float angular = znet::ext::ReadAngularVelocity(bits, quantization);
```

`Body2DQuantization` holds the world bounds and field widths. **Both ends must
agree on it**; it is not sent, because sending it every tick would cost more
than the fields it describes. The defaults suit a world about two thousand units
across, landing positions within 3 cm and rotation within 0.35°.

## Rotation has no seam

The angle wraps, so +π and −π get the same code. Without that, a body spinning
through π would jump a whole quantisation step every revolution — a visible
stutter that only appears at one orientation, which is a miserable thing to
debug. There is a test for it.

## Tests

`ctest -R ext-box2d-tests`. 5000 random transforms round-tripped, the seam
swept at quarter-step increments either side of π, and 20000 random bit
patterns confirming every decoded `b2Rot` is unit length.

Verified at C++14 and above, on GCC and Clang, against Box2D 3.1.
