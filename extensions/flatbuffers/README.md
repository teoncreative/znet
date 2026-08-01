# znet-flatbuffers

[FlatBuffers](https://github.com/google/flatbuffers) payloads in a `Buffer`,
verified before anything reads them.

Header-only. Everything lives in `znet::ext`. **Requires C++17**, which is the
FlatBuffers runtime's own floor.

```cmake
target_link_libraries(my_game PRIVATE znet-flatbuffers)   # or znet::flatbuffers
```

```cpp
#include "znet/ext/flatbuffers/flatbuffers.h"
```

## The one rule

FlatBuffers is fast because field accessors are raw pointer arithmetic over the
payload, with no bounds checks whatsoever. That is fine for a buffer you built.
For a buffer that arrived over the network it means a payload whose internal
offsets point outside itself turns `GetRoot<T>()->field()` into an arbitrary
read.

FlatBuffers ships a `Verifier` for exactly this, and forgetting to run it is
easy and silent, because malformed buffers usually appear to work.

So this extension gives you no way to obtain a root without one having run.
`ReadFlatBuffer<T>` verifies and returns `false` if it fails;
`FlatBufferSerializer<T>` verifies and drops the packet if it fails.

## Usage

```cpp
flatbuffers::FlatBufferBuilder builder;
builder.Finish(CreatePlayer(builder, 42, builder.CreateString("player one")));

auto buffer = std::make_shared<Buffer>();
znet::ext::WriteFlatBuffer(*buffer, builder);
```

```cpp
std::vector<uint8_t> bytes;
if (!znet::ext::ReadFlatBuffer<Player>(*buffer, bytes)) {
  return;   // oversized, truncated, or did not verify
}
const Player* player = znet::ext::GetVerifiedRoot<Player>(bytes);
```

As a packet:

```cpp
codec->Add(kPacketState,
           std::make_unique<znet::ext::FlatBufferSerializer<Player>>(kPacketState));

auto packet = std::make_shared<znet::ext::FlatBufferPacket<Player>>(kPacketState);
packet->SetFrom(builder);
session->SendPacket(packet);

// on receipt, already verified:
packet->Get()->level();
```

## Why reading copies

`ReadFlatBuffer` hands back a `std::vector<uint8_t>` that owns the payload, for
two reasons that both matter:

- **Lifetime.** Accessors point into the payload, so it has to outlive every
  read, and the `Buffer` it arrived in gets recycled.
- **Alignment.** FlatBuffers requires the buffer to start on a suitably aligned
  address. A payload sitting behind a varint length prefix is at an arbitrary
  offset, so it is not. The copy lands on an allocator-aligned address, and the
  read checks that rather than assuming it: the verifier's own alignment
  checking is relative to the start of the buffer, so it cannot notice that the
  start itself is misaligned.

If you need true zero-copy, frame the payload so it is already aligned in the
receive buffer and call the verifier yourself. This extension chooses one copy
per packet over an easy way to get memory-unsafe.

## Limits

```cpp
znet::ext::FlatBufferLimits limits;
limits.max_bytes  = 64 * 1024;   // default 1 MiB, checked before allocating
limits.max_depth  = 64;          // nesting of tables and vectors
limits.max_tables = 1000000;
```

The depth and table bounds are the verifier's own; this just surfaces them. The
size bound is checked against the varint prefix *and* against the bytes actually
present, so a length field claiming four gigabytes is refused rather than
reserved.

## Building

Uses an installed FlatBuffers if there is one, otherwise fetches v25.2.10 with
`FLATBUFFERS_BUILD_FLATC=OFF`. Only the runtime headers are wanted here: `flatc`
is a code generator, and it belongs in whatever step compiles your `.fbs` files,
not in a build that does not use it. The fetched copy is marked `SYSTEM`.

```
-DZNET_EXT_FLATBUFFERS=OFF  just this extension off
-DZNET_EXT_ALLOW_FETCH=OFF  skip rather than download
```

## Tests

`ctest -R ext-flatbuffers-tests`. The tests define their generated-style table
by hand, so they need only the runtime headers and no `flatc` in the build.

Beyond round trips, the suite flips every bit pattern at every byte position of
a valid payload and requires each result either to be refused or to be safe to
walk, and throws 3000 random byte strings at the verifier. Both run under
AddressSanitizer and UndefinedBehaviorSanitizer in CI-equivalent local runs,
which is what makes "safe to walk" a checked claim rather than an assertion.

Verified at C++17, 20 and 23, on GCC and Clang.
