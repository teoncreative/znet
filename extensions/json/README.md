# znet-json

Length-prefixed [nlohmann-json](https://github.com/nlohmann/json) in a `Buffer`,
with the bounds that make parsing network-supplied json safe, plus a ready-made
`PacketSerializer`.

Header-only. Everything lives in `znet::ext`.

```cmake
target_link_libraries(my_game PRIVATE znet-json)   # or znet::json
```

```cpp
#include "znet/ext/json/json.h"
```

## What it is for

Control-plane traffic: lobby and matchmaking messages, server configuration,
RPC-shaped requests, debug commands, anything schema-loose or low-rate. Its
virtue is that you can add a field without touching the other end.

**Not per-tick game state.** A json position is an order of magnitude larger
than [`znet-glm`](../glm/)'s six bytes, and parsing it costs far more than
reading a float. Use json where flexibility matters and bandwidth does not.

## Usage

```cpp
auto buffer = std::make_shared<Buffer>();
znet::ext::WriteJson(*buffer, nlohmann::json{{"map", "dust"}, {"players", 10}});

nlohmann::json config;
if (!znet::ext::ReadJson(*buffer, config)) {
  return;   // oversized, truncated, too deep, or not valid MessagePack
}
```

As a packet, registered like any other serializer:

```cpp
codec->Add(kPacketConfig,
           std::make_unique<znet::ext::JsonSerializer>(kPacketConfig));

auto packet = std::make_shared<znet::ext::JsonPacket>(kPacketConfig);
packet->body = {{"map", "dust"}, {"players", 10}};
session->SendPacket(packet);
```

`JsonSerializer::DeserializeTyped` returns `nullptr` on anything malformed,
which is how a `PacketSerializer` says "drop this", so a hostile document costs
one packet rather than the session.

| | Wire form | Use |
| --- | --- | --- |
| `WriteJson` / `ReadJson` | MessagePack | default; roughly half the size and quicker to parse |
| `WriteJsonText` / `ReadJsonText` | UTF-8 text | when a packet capture has to be readable by a human |

Both are length-prefixed with a varint, so a json document sits among ordinary
`Buffer` fields inside a larger packet.

## Limits, and why the depth one is not optional

```cpp
znet::ext::JsonLimits limits;
limits.max_bytes = 64 * 1024;   // default 1 MiB
limits.max_depth = 16;          // default 64
znet::ext::ReadJson(*buffer, out, limits);
```

nlohmann's **MessagePack reader is recursive**, and it is reached directly from
network bytes. A payload of a hundred thousand repeated `0x91` bytes, about a
hundred kilobytes on the wire, nests a hundred thousand arrays and overflows
the stack. That is a remote crash from a small packet. Measured, not
theoretical: calling `from_msgpack` on it segfaults.

Interestingly its *text* parser is iterative and shrugs off millions of levels,
so this is specific to the binary path, which is exactly the one a wire format
wants to use.

So every decode here is a two-pass operation. The first pass is a SAX consumer
that counts nesting and builds nothing; returning false from `start_array`
makes the reader unwind instead of descending, so the scan is bounded by
`max_depth` no matter what the payload claims. Only a payload that survives
that gets parsed for real. A wide document is unaffected: depth is counted, not
inferred from length, so a 20000-element array passes at `max_depth = 4`.

The size limit is checked against the varint prefix *before* anything is
allocated, and against the bytes actually present, so a length field claiming
four gigabytes is refused rather than reserved.

## Nothing throws

nlohmann throws by default, and an exception escaping into znet's decode path
would take down the session rather than the packet. Every read here uses the
non-throwing parse and reports through a `bool`; every write handles a
discarded value rather than throwing on it. The test suite asserts this over
6000 random byte and text payloads.

A failed read leaves the destination untouched, so a decode into a live config
object cannot half-overwrite it.

## Building

Uses an installed nlohmann-json if there is one, otherwise fetches 3.12.0. It
fetches the **release** tarball rather than the repository one: 115 KB against
tens of megabytes of test fixtures, carrying the same CMake project. The
fetched copy is marked `SYSTEM`.

```
-DZNET_EXT_JSON=OFF         just this extension off
-DZNET_EXT_ALLOW_FETCH=OFF  skip rather than download
```

## Tests

`ctest -R ext-json-tests`. Round trips for both wire forms, the depth guard at
and just past its limit, a 200000-deep payload that would otherwise crash the
process, oversized and truncated and implausible-length payloads, and 6000
random payloads asserting that decoding always returns a verdict instead of
throwing.

Verified at C++14, 17, 20 and 23, on GCC and Clang.
