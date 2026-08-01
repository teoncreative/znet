# znet-delta

Send only the fields that changed since a baseline the peer has acknowledged.

Header-only, **no third-party dependency**, but it builds on
[`znet-bitpack`](../bitpack/): a per-field "changed" flag is only worth having
when a flag costs one bit rather than a byte.

```cmake
target_link_libraries(my_game PRIVATE znet-delta)   # or znet::delta
```

```cpp
#include "znet/ext/delta/delta.h"
```

## Three pieces

| | What it does |
| --- | --- |
| `SnapshotHistory<T>` | Keeps recent snapshots and tracks what the peer acknowledged, so you can pick a baseline it genuinely has |
| `DeltaWriter` | Writes one bit per unchanged field, the value only when it moved |
| `DeltaReader` | Rebuilds the snapshot from a baseline plus what arrived |

Plus `SequenceGreaterThan` / `SequenceLessThan` / `SequenceDifference` for
counters that wrap.

## Usage

Per tick on the sender:

```cpp
history.Store(sequence, snapshot);

const Snapshot* baseline = history.AcknowledgedSnapshot();
if (baseline != nullptr) {
  buffer->WriteInt<uint16_t>(sequence);
  buffer->WriteInt<uint16_t>(history.acknowledged());   // name the baseline
  BitWriter bits(*buffer);
  DeltaWriter delta(bits);
  delta.WriteUIntRanged(snapshot.ammo,   baseline->ammo,   0, 200);
  delta.WriteIntRanged (snapshot.health, baseline->health, -50, 100);
  delta.WriteFloatRanged(snapshot.yaw,   baseline->yaw,   -3.15f, 3.15f, 12);
  delta.WriteBool(snapshot.firing);
} else {
  WriteFullSnapshot(*buffer, snapshot);
}
```

and whenever an ack comes back, `history.Acknowledge(their_sequence)`.

On the receiver, the same calls in the same order with the same bounds, against
the baseline the packet named:

```cpp
DeltaReader delta(bits);
now.ammo   = delta.ReadUIntRanged(was.ammo,   0, 200);
now.health = delta.ReadIntRanged (was.health, -50, 100);
now.yaw    = delta.ReadFloatRanged(was.yaw,  -3.15f, 3.15f, 12);
now.firing = delta.ReadBool();
if (!bits.ok()) return nullptr;
```

Framing is left to you, since packet layout is a game's own decision. All the
extension needs is that the receiver can tell which baseline a delta was built
against.

## What it saves

From the lossy-link test in `tests/delta_test.cc`: a six-field player state
costs **50 bits** in full, and **24.9 bits on average** as a delta over 3000
ticks with 20% packet loss. An entity that did not move at all costs **6 bits**,
one flag per field.

The win scales with how static your state is, which in practice is very: most
entities in most ticks are doing nothing.

## The two rules that make it correct

Delta compression fails in a specific and nasty way. A wrong baseline does not
produce a corrupt packet the receiver can reject; it produces a receiver that
is confidently, silently wrong. Both rules below exist to prevent that.

**Delta against what the peer acknowledged, never against the previous tick.**
Over a lossy link the previous snapshot is often not what the peer holds.
`AcknowledgedSnapshot()` returning `nullptr` is the normal signal to send a full
snapshot, meaning either the peer has acknowledged nothing yet or its baseline
has aged out of the window. It is not an error. A sender that treats it as one
stalls; a sender that ignores it and deltas against something else corrupts the
receiver.

**For lossy fields, "unchanged" means the quantised code is unchanged.** What
the receiver holds is a dequantised code, not your float. `WriteFloatRanged`
compares codes rather than floats, so a value drifting inside one quantisation
step costs one bit instead of a whole field, and a caller whose baseline is its
own full-precision value cannot accidentally skip a field the receiver needed.
`ReadFloatRanged` requantises the baseline for the same reason, which is what
keeps a field held still for ten thousand ticks from creeping.

## Fields

| Writer | Reader | Unchanged | Changed |
| --- | --- | --- | --- |
| `WriteUIntRanged(v, base, min, max)` | `ReadUIntRanged(base, min, max)` | 1 bit | 1 + `BitsForRange` |
| `WriteIntRanged(v, base, min, max)` | `ReadIntRanged(base, min, max)` | 1 bit | 1 + `BitsForSignedRange` |
| `WriteFloatRanged(v, base, min, max, n)` | `ReadFloatRanged(base, min, max, n)` | 1 bit | 1 + n |
| `WriteBits(v, base, n)` | `ReadBits(base, n)` | 1 bit | 1 + n |
| `WriteBool(v)` | `ReadBool()` | 1 bit | 1 bit |

`WriteBool` deliberately takes no baseline. A changed flag costs one bit and so
does the value, so delta encoding a bool can only make it bigger. Taking the
parameter and ignoring it would just invite the next reader to assume otherwise.

Unchanged reads return the baseline **clamped to the bounds**, which is what the
sender compared against, so a baseline that has drifted out of range cannot make
the two ends disagree.

`DeltaWriter::fields_written()` and `fields_changed()` are there for tuning:
they tell you which fields are actually costing you bandwidth.

## Sequence numbers

A uint16 counter at 60 Hz wraps every 18 minutes, and after it does, plain `<`
is wrong in the direction that matters. `SequenceGreaterThan(0, 65535)` is
true. Every comparison treats half the sequence space as the horizon, so it is
only meaningful for values genuinely within half a period of each other; a peer
quieter than that should have its baseline treated as gone.

`SnapshotHistory` slots carry their own sequence number, so a counter that has
come all the way round reports a miss rather than handing back a stale snapshot
under a new number.

## Not done yet

**Small-delta encoding.** When a value usually moves by a little, the change
itself is cheaper than the value: ammo going 137 to 136 could cost a 4-bit
signed difference plus an escape flag rather than the full 8-bit field. Worth
adding for positions in particular.

**Per-entity delta over a set.** This extension deltas one struct against one
baseline. A snapshot of *n* entities also wants to encode which entities
appeared and disappeared, which is a layer up.

## Tests

`ctest -R ext-delta-tests`. The one that matters is `SurvivesALossyLink`: 3000
ticks with 20% loss, checking on every delivered packet that the receiver's
decoded state matches the sender's truth exactly, and that sender and receiver
never disagree about which baseline is in play. Also covers wrap-around
ordering across the whole 65536-value space, and a field held still for 10000
ticks not drifting.

Verified at C++14, 17, 20 and 23, on GCC and Clang.
