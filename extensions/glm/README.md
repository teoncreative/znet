# znet-glm

Buffer read/write for [glm](https://github.com/g-truc/glm)'s vector, matrix and
quaternion types, plus the lossy encodings a game actually wants for state that
goes out every tick.

Header-only. Everything lives in `znet::ext`.

```cmake
target_link_libraries(my_game PRIVATE znet-glm)   # or znet::glm
```

```cpp
#include "znet/ext/glm/glm.h"      // everything
#include "znet/ext/glm/serialize.h"  // exact forms only, no glm packing headers
```

## Exact serialization

`WriteVec` / `WriteMat` / `WriteQuat` and their readers round-trip bit-for-bit
and respect the buffer's endianness.

```cpp
class TransformSerializer : public PacketSerializer<TransformPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<TransformPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint32_t>(packet->entity_id);
    znet::ext::WriteVec(*buffer, packet->position);
    znet::ext::WriteQuat(*buffer, packet->rotation);
    return buffer;
  }

  std::shared_ptr<TransformPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<TransformPacket>();
    packet->entity_id = buffer->ReadInt<uint32_t>();
    packet->position = znet::ext::ReadVec<glm::vec3>(*buffer);
    packet->rotation = znet::ext::ReadQuat<glm::quat>(*buffer);
    if (buffer->GetAndClearLastError() != BufferError::None) {
      return nullptr;   // truncated or malformed frame
    }
    return packet;
  }
};
```

Every reader has two spellings: `ReadVec(buffer, out)` fills a value you
already have, `ReadVec<glm::vec3>(buffer)` returns one.

| Function | Works on | Notes |
| --- | --- | --- |
| `WriteVec` / `ReadVec` | any `glm::vec<L, T, Q>` | vec2/3/4, dvec, ivec, uvec, bvec, sized variants |
| `WriteMat` / `ReadMat` | any `glm::mat<C, R, T, Q>` | column-major, matching glm's storage |
| `WriteQuat` / `ReadQuat` | any `glm::qua<T, Q>` | x, y, z, w by name |
| `WriteVecArray` / `ReadVecArray` | `std::vector<glm::vec<...>>` | varint count, then the elements |

Errors follow the core Buffer convention: a read that runs off the end records
`ReadOutOfBounds` on the buffer, so you check once per packet rather than once
per field. `ReadVecArray` is the exception and returns `bool`, because it
decides how much to allocate from a count that came off the wire.

## Compressed encodings

Reach for these on the fields that go to every peer every tick. The reader must
call the `Read*` matching the `Write*` the sender used; nothing here is
self-describing.

| Field | Exact | Compressed | Function | Worst-case error |
| --- | --- | --- | --- | --- |
| position | 12 B | 6 B | `WriteVecRanged<uint16_t>` over the world box | half a step, e.g. 1.5 cm over 2 km |
| position | 12 B | 6 B | `WriteVecHalf` | relative, ~0.1% |
| rotation | 16 B | **4 B** | `WriteQuatSmallestThree` | **0.115°**, identity is exact |
| normal | 12 B | 4 B | `WriteNormalOct<uint16_t>` | 0.0037° |
| normal | 12 B | 2 B | `WriteNormalOct<uint8_t>` | 0.94° |

A transform update of position + rotation + normal goes from **40 bytes to 14**.

Error figures are measured over 300k uniform samples and asserted in
`tests/glm_quantize_test.cc`, so they are bounds the suite defends rather than
estimates.

### Choosing between half and ranged

`WriteVecHalf` costs the same as `WriteVecRanged<uint16_t>` and needs no bounds,
but a half's precision is relative: millimetres near the origin, two metres at
1024 units out. For world positions use `WriteVecRanged`, whose step is uniform
everywhere. Halves are the right call for velocities, local offsets and colours.

### Robustness

Decoders never trust their input. Any 32-bit word decodes to a finite unit
quaternion, and any pair of bytes decodes to a unit normal, so a decoded value
can go straight into a matrix without being checked first.

## Wire format

Fixed layouts, no headers, no padding.

- **vec**: components in x, y, z, w order, each in the buffer's endianness.
  Length is not sent; it is part of the type on both ends.
- **mat**: columns in order, each column as a vec. A `mat4` is 16 floats in
  the same order glm stores them.
- **quat**: x, y, z, w, addressed *by name*. `GLM_FORCE_QUAT_DATA_XYZW`
  changes what `q[0]` means, and two peers may have built glm differently, so
  the format is pinned to the maths rather than the memory layout.
- **vec array**: varint count, then that many elements.
- **smallest-three quat**: one big-endian-independent `uint32`, with 2 bits naming
  the dropped (largest) component, then the other three at 10 bits each in
  ascending component order, mapped from `[-1/√2, 1/√2]`. 1023 levels, not
  1024: an even count puts zero between two codes, which would make the
  identity quaternion decode as a 0.137° rotation and a resting object jitter.
- **octahedral normal**: two unsigned ints of the chosen width, each a
  fixed-point value over `[-1, 1]`.

`bool` components cross the wire as a normalised `uint8`, not a raw byte. A
`bool` holding anything but 0 or 1 is undefined, since both `b` and `!b` can
test true, and a peer can trivially send such a byte.

## Building

Uses an installed glm if there is one, since your renderer is already built
against a specific version and a second copy on the include path helps nobody.
Otherwise it fetches glm 1.0.1, unless `-DZNET_EXT_ALLOW_FETCH=OFF`, in which
case the extension skips itself.

```
-DZNET_EXT_GLM=OFF          just this extension off
-DZNET_BUILD_EXTENSIONS=OFF all extensions off
```

Tests build only when the parent tree has already set up googletest, and run as
`ctest -R ext-glm-tests`. They are compiled with the same warning set as the
core library.

Verified against glm 1.0 at C++14, 17, 20 and 23, on GCC and Clang.
