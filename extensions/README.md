# znet extensions

Optional add-ons that bridge znet to a third-party library. Each one is
self-contained, links znet the way any consumer would, and adds nothing to the
core target: a build with every extension off is byte-identical to a build
from before they existed.

They live here rather than in `znet/` because they carry dependencies the
library itself refuses to take on. znet needs OpenSSL and zstd; it should not
need a maths library because some users serialize vectors.

## Available

| Extension | Target | Dependency | What it gives you |
| --- | --- | --- | --- |
| [quantize](quantize/) | `znet-quantize` | none | The quantisation arithmetic on plain scalars. Every other extension that compresses a value delegates here, so all of them agree bit for bit |
| [reflect](reflect/) | `znet-reflect` | none, **C++17** | Serialize a struct from its fields, with nothing declared for plain aggregates. `ZNET_REFLECT` for the rest |
| [bitpack](bitpack/) | `znet-bitpack` | `znet-quantize` | Sub-byte fields in a Buffer: a bool costs 1 bit, a 0-200 value costs 8. Ranged ints and floats that compute their own width |
| [delta](delta/) | `znet-delta` | `znet-bitpack` | Send only what changed since a baseline the peer acknowledged. Snapshot history, wrap-safe sequence numbers, per-field change flags |
| [box2d](box2d/) | `znet-box2d` | Box2D 3.x | Planar body state in 10 bytes instead of 28. Orientation is one wrapped angle, not a quaternion |
| [bullet](bullet/) | `znet-bullet` | Bullet 3.x | Rigid body state in 15 bytes instead of 40 |
| [jolt](jolt/) | `znet-jolt` | Jolt 5.x, **C++17** | Rigid body state in 15 bytes instead of 40 |
| [entt](entt/) | `znet-entt` | EnTT ≥ 3.15, **C++17** | Buffer archives for `entt::snapshot` and `continuous_loader`, so an ECS registry replicates with entity remapping handled |
| [flatbuffers](flatbuffers/) | `znet-flatbuffers` | FlatBuffers ≥ 23, **C++17** | Verified FlatBuffers payloads: no way to get a root without the Verifier having run |
| [glm](glm/) | `znet-glm` | glm ≥ 0.9.9, `znet-quantize` | Buffer read/write for `vec`/`mat`/`quat`, plus compressed forms: 4-byte quaternions, 4-byte normals, ranged fixed-point positions |
| [spdlog](spdlog/) | `znet-spdlog` | spdlog ≥ 1.x | Routes znet's logging into spdlog with the severity intact, rather than as a pre-formatted line |
| [json](json/) | `znet-json` | nlohmann-json ≥ 3.11 | Length-prefixed json (MessagePack or text) with depth and size bounds that make untrusted json safe to parse, plus a ready-made `PacketSerializer` |

## Building

On by default. Each extension skips itself with a `message(STATUS ...)` when
its dependency is missing, so leaving them enabled cannot break a build.

```
-DZNET_BUILD_EXTENSIONS=OFF   all extensions off
-DZNET_EXT_ALLOW_FETCH=OFF    never download a missing dependency; skip instead
-DZNET_EXT_<NAME>=OFF         one extension off
```

Tests are wired only when the parent tree has already set up googletest, and
run under the usual `ctest`. An extension never fetches a test framework of its
own, so vendoring a single extension directory into another project gets you
the headers and nothing else.

## Adding one

- header-only `INTERFACE` target where possible, named `znet-<thing>`, with a
  `znet::<thing>` alias
- public headers under `include/znet/ext/<thing>/`, umbrella header
  `<thing>.h` next to them
- everything in namespace `znet::ext`
- C++14 is the floor, matching the core library, so `target_compile_features(...
  INTERFACE cxx_std_14)` and no C++17-only spellings. Use `znet/compat.h` for
  anything newer.
- prefer an installed copy of the dependency over fetching one; a consumer's
  renderer is already built against a specific version and a second copy on the
  include path helps nobody
- `return()` early with a `message(STATUS ...)` if the dependency is absent
- tests under `tests/`, guarded on `if(TARGET gtest_main)`, compiled with the
  same warning set as the core library
- a README with the wire format, because that is the part consumers have to
  match on the other end

Then add `add_subdirectory(<thing>)` here and a row to the table above.

## Ideas not yet built

Roughly in order of how much they would earn their keep:

- **Entity-set snapshots.** `znet-delta` deltas one struct against one
  baseline. A snapshot of many entities also needs to say which ones appeared
  and disappeared since the baseline, and to spend its bandwidth budget on the
  entities that matter most to each viewer. That is the layer above.
- **Dear ImGui.** A debug panel over the metrics already collected in
  `metrics.h`: RTT, congestion window, per-packet-type counters.
- **Protobuf.** A `PacketSerializer` adapter so a schema-generated type drops
  into znet's pipeline without a hand-written serializer.
