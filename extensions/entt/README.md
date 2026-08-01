# znet-entt

Buffer-backed archives for [EnTT](https://github.com/skypjack/entt)'s snapshot
machinery, so an ECS registry replicates over znet.

Header-only. Everything lives in `znet::ext`.

```cmake
target_link_libraries(my_game PRIVATE znet-entt)   # or znet::entt
```

```cpp
#include "znet/ext/entt/entt.h"
```

**Requires C++17**, which is EnTT's own floor. On a `-DZNET_CXX_STANDARD=14`
tree this extension skips itself and says so, rather than failing the build.

## What it is, and what it deliberately is not

EnTT already knows how to walk a registry, and, more importantly, how to remap
incoming entity identifiers onto locally allocated ones via
`entt::continuous_loader`. This extension supplies the missing piece: somewhere
to put the bytes.

That narrowness is the design. A client's registry is already using identifiers
of its own, so a server's identifiers almost always collide with something
local. Remapping them, and keeping the mapping stable across updates, is where
a hand-rolled ECS replication layer goes wrong, and `continuous_loader` is
already correct. Reimplementing it here would only add a second thing to get
wrong.

## Usage

Sending:

```cpp
znet::ext::EnttOutputArchive archive(*buffer);
entt::snapshot{registry}
    .get<entt::entity>(archive)
    .get<Position>(archive)
    .get<Health>(archive)
    .get<Team>(archive);
```

Receiving into a client registry that has entities of its own:

```cpp
entt::continuous_loader loader{client_registry};   // keep this across updates
znet::ext::EnttInputArchive archive(*buffer);
loader.get<entt::entity>(archive)
      .get<Position>(archive)
      .get<Health>(archive)
      .get<Team>(archive);

if (!archive.ok()) {
  // the packet was truncated or claimed more than it carried
}

const entt::entity local = loader.map(server_entity);
```

Use `entt::snapshot_loader` instead when the target registry is empty and the
identifiers can be taken as-is.

## Saying how a component is serialized

The archive cannot know your component layout, so it uses a customization
point. Define these beside the component and argument-dependent lookup finds
them:

```cpp
namespace game {

struct Position { float x, y, z; };

void SerializeComponent(znet::Buffer& buffer, const Position& value) {
  buffer.WriteFloat(value.x);
  buffer.WriteFloat(value.y);
  buffer.WriteFloat(value.z);
}

void DeserializeComponent(znet::Buffer& buffer, Position& value) {
  value.x = buffer.ReadFloat();
  value.y = buffer.ReadFloat();
  value.z = buffer.ReadFloat();
}

}  // namespace game
```

Already handled: arithmetic types, enums (as their underlying type),
`std::string`, and `bool`. Anything else without an overload is a **compile
error naming the type**, rather than a silent `memcpy` of whatever padding the
struct happens to contain.

`bool` is not left to the arithmetic path on purpose. `ReadNumber` copies the
raw byte, and a `bool` holding anything but 0 or 1 is undefined, so both `b`
and `!b` can test true. A peer can trivially send such a byte.

Empty components work as tags with no overload needed: EnTT stores no payload,
so the archive is never called with one and only the entity list crosses.

## Safety

A snapshot's counts are attacker-controlled, and EnTT uses them directly to
reserve storage and to bound its own loops. It has no way to know they are
implausible. So the input archive **clamps every count against what the buffer
actually holds**: each element EnTT goes on to read costs at least one 4-byte
entity identifier, so a count above `readable_bytes / 4` cannot be honest.

Without that, a nine-byte packet claiming four billion entities is both a huge
allocation and a four-billion-iteration loop. With it, the same packet becomes
a short read the loader survives, and `ok()` returns false.

`ok()` false means the registry holds whatever arrived before the problem, so
load into a scratch registry rather than the live one if that matters.

## Wire format

Whatever `entt::snapshot` emits, with:

- **entity identifiers**: fixed 4 bytes. Not a varint, because EnTT writes the
  null entity, which is all bits set, to mark gaps in the entity list, and that
  is precisely a varint's worst case.
- **lengths and free-list counts**: varint.
- **components**: whatever your `SerializeComponent` writes.

Written for the default 32-bit `entt::entity` registry.

## Building

Uses an installed EnTT if there is one, otherwise fetches 3.15.0. The fetched
copy is marked `SYSTEM`, so EnTT's own warnings do not surface in your build.

```
-DZNET_EXT_ENTT=OFF         just this extension off
-DZNET_EXT_ALLOW_FETCH=OFF  skip rather than download
```

## Tests

`ctest -R ext-entt-tests`. Covers round trips including destroyed-entity gaps
and tag components, `continuous_loader` remapping onto a registry already
holding 40 entities of its own, mapping staying stable across three successive
updates, and hostile input: an absurd count, a truncated snapshot, and 300
random byte strings that must all terminate without a wild allocation.

Verified against EnTT 3.15 at C++17, 20 and 23, on GCC and Clang.
