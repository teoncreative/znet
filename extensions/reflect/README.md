# znet-reflect

Serialize a struct from its fields. For a plain aggregate there is nothing to
declare at all.

Header-only, **no third-party dependency**. Everything lives in `znet::ext`.
**Requires C++17.**

```cmake
target_link_libraries(my_game PRIVATE znet-reflect)   # or znet::reflect
```

```cpp
#include "znet/ext/reflect/reflect_all.h"
```

## The short version

```cpp
struct Player {
  uint32_t id;
  std::string name;
  float health;
  std::vector<uint32_t> items;
};

znet::ext::WriteAuto(*buffer, player);

Player read;
if (!znet::ext::ReadAuto(*buffer, read)) { /* truncated or malformed */ }
```

No macro, no schema, no hand-written serializer to keep in step with the
struct. As a packet:

```cpp
codec->Add(kMove, znet::ext::MakeAutoSerializer<Move>(kMove));

auto packet = std::make_shared<znet::ext::AutoPacket<Move>>(kMove, move);
session->SendPacket(packet);
```

## How it works, and why C++17

C++ has no reflection before C++26, so the field list has to come from
somewhere. C++17 has just enough to deduce it for aggregates:

- **Field count** by asking which brace-initialiser arities compile, using a
  probe type that converts to anything.
- **Field access** by structured bindings, `auto& [a, b, c] = value`.

Neither is available in C++14, which is why this extension asks for 17 and
steps aside on a C++14 tree rather than dragging the whole build up. `if
constexpr` then collapses the type dispatch into one readable chain instead of
a SFINAE overload set, which is a nice second-order win rather than the reason.

What deduction **cannot** do is recover field *names* (that needs C++26), and
it only works on aggregates: no user-declared constructors, no private members,
no base classes, no virtuals.

## When deduction is not enough

`ZNET_REFLECT` names the fields by hand, and takes precedence over deduction:

```cpp
class Session {
 public:
  Session(uint64_t token, std::string label);
  uint64_t token;
  std::string label;
};
ZNET_REFLECT(game::Session, token, label)   // at global scope, type in full
```

Three reasons to reach for it:

- the type is not an aggregate
- you want a field **left off the wire**, which declaring the others achieves
- you want field names available to a visitor, for a debug dump

Nested aggregates deduce correctly, including a struct-of-structs. That is
worth stating because it is the case brace elision would be expected to break;
it does not, because the probe converts straight to the member's own type and
so never elides. There are tests for it.

## Supported field types

| | |
| --- | --- |
| arithmetic | via `Buffer::WriteNumber`, so the buffer's endianness applies |
| `bool` | as a normalised byte, never a raw one |
| enums | as their underlying type |
| `std::string` | varint length, then bytes |
| `std::vector<T>` | varint count, then elements |
| `std::array<T, N>` | elements only; the arity is part of the type |
| `std::map`, `std::unordered_map` | varint count, then key/value pairs |
| nested aggregates and reflected types | recursively |

Anything else is a **compile error naming the type**, rather than a silent
`memcpy` of whatever padding the struct happens to contain. Add
`WriteValue`/`ReadValue` overloads, or a `ZNET_REFLECT`, to extend it.

`bool` is not folded into the arithmetic case on purpose. `ReadNumber` copies
the raw byte, and a `bool` holding anything but 0 or 1 is undefined (both `b`
and `!b` can test true), and a peer can trivially send such a byte.

## Limits

```cpp
znet::ext::ReflectLimits limits;
limits.max_elements = 4096;   // default 1 Mi, per container or string
znet::ext::ReadAuto(*buffer, value, limits);
```

Every container length is attacker-controlled and about to size an allocation
and bound a loop. Each is checked against `max_elements` **and** against the
bytes actually present: an element costs at least one byte whatever its type,
so a count above `readable_bytes` cannot be honest. That is what stops a
nine-byte packet asking for a gigabyte.

`ReadAuto` also consults the buffer's own error state before returning true, so
a field that ran off the end is caught even when no length looked wrong.

## The catch worth knowing

The struct **is** the schema. Add a field and both ends change together, which
is the point. It also means a field added on one side and not the other is a
wire break with nothing to detect it. There is no version tag and no field
identifier on the wire; it is a positional format, like the rest of znet's
serialization.

Bump the packet id when the struct changes, or use
[`znet-json`](../json/) or [`znet-flatbuffers`](../flatbuffers/) where the two
ends genuinely evolve apart.

Field order is wire order, so reordering members changes the format even though
the code looks the same.

## Tests

`ctest -R ext-reflect-tests`. Round trips for every supported category, nested
aggregates, vectors of aggregates, a declared non-aggregate, a field kept off
the wire, truncation, implausible container and string lengths, the element
ceiling, and 5000 random byte strings checked to decode without allocating
beyond the payload.

Verified at C++17, 20 and 23, on GCC and Clang.
