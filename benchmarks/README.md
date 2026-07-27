# znet benchmarks

Benchmarks for znet's two transports, a raw-socket floor and three comparison
libraries, on clean loopback and under netem impairment. Every library runs the same workloads through the same
reporting code (`common/harness.h`), so rows from different binaries line up
into one table.

**Results are in the [root README](../README.md#benchmarks).**

## Running

Benchmarks are off by default:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DZNET_BUILD_BENCHMARKS=ON
cmake --build build --target benchmarks

# in a namespace of its own, so the traffic stays off the host's loopback
unshare -rn sh -c 'ip link set lo up
  taskset -c 0-7,16-23 ./build/benchmarks/znet-bench'
```

Use a namespace even when not impairing anything: these saturate loopback and
znet starts `hardware_concurrency()` worker threads, which otherwise competes
with anything of yours on localhost. A fresh namespace's `lo` is the same 65536
MTU, so the numbers are unaffected.

Pick the core list to sit inside one L3 domain. On a multi-CCD part a set that
straddles the boundary lets client and server land on different dies from run
to run, and the extra hop shows up as a bimodal result rather than as noise:
the raw socket baselines split into two clusters about 2x apart that way. Check
yours with

```sh
cat /sys/devices/system/cpu/cpu0/cache/index3/shared_cpu_list
```

and use a list from a single group. The `0-7,16-23` above is one domain of a
Ryzen 9 9950X3D, 8 physical cores and their SMT siblings.

Comparison libraries are downloaded at configure time and enabled individually,
so one that stops building does not block the rest:

```sh
-DZNET_BENCH_ENET=ON      # small, clean CMake, builds anywhere
-DZNET_BENCH_RAKNET=ON    # archived upstream, needs -fpermissive (handled)
-DZNET_BENCH_GNS=ON       # Valve GameNetworkingSockets, needs protobuf + OpenSSL
```

GNS enables C and pulls a large dependency tree; configure it in a separate
build directory if you want to keep the normal one lean.

## Impaired runs

To reach the regimes that separate these protocols, put netem on the loopback
of an unprivileged network namespace and tell the benchmark what you did:

```sh
unshare -rn sh -c '
  ip link set lo mtu 1500
  ip link set lo up
  tc qdisc add dev lo root netem delay 25ms loss 5%
  ZNET_BENCH_IMPAIR="delay=25,loss=5" \
    taskset -c 0-7,16-23 ./build/benchmarks/znet-bench'
```

No root, and the host's networking is untouched. `delay` and `jitter` are one
way, so a round trip pays each twice.

The environment variable does not impair anything - netem does. It exists
because nothing can read netem's settings back, and the benchmark needs them
for two things: scaling the workloads (the stock 2,000 ping-pongs is ten
minutes at a 300 ms round trip) and labelling the output. Give it the same
numbers you gave netem or the counts will be wrong.

netem rather than a forwarder inside the benchmark, because it sits below the
socket and so impairs TCP too. A userspace forwarder can only corrupt a proxied
TCP stream, not drop from it, and would leave the TCP rows quietly unimpaired.

### Why the clean tables flatter TCP

Loopback's MTU is 65536 and the kernel does not really segment on it: a TCP
write there is a copy between socket buffers, with no reliability bookkeeping in
userspace and no syscall per datagram. Dropping the MTU to 1500 moves raw TCP by
about 2% at 8 KiB but halves raw UDP, which then pays IP fragmentation while TCP
still does not.

So the raw TCP row is a floor, not a rival, and its 8 KiB and latency figures
describe a memcpy rather than a network.

## Workloads

| case | payload | messages | what it exercises |
| --- | --- | --- | --- |
| 64B | 64 B | 50000 | per-message overhead; small enough that a datagram could hold many |
| 1KB | 1 KiB | 20000 | roughly one message per MTU-sized datagram |
| 8KB | 8 KiB | 5000 | forces fragmentation in a datagram transport |
| latency | 64 B | 2000 | ping-pong round trip, echoed by the peer |

Throughput counts messages the *receiver* actually delivered to the
application, not messages handed to the sender, so silent loss shows up as a
low count rather than a high rate.

**Payloads are incompressible by default** (`bench::MakePayload`): one repeated
byte flatters anything that compresses, since zstd takes 64 bytes of 'x' down to
about 15. `PayloadKind` also offers `Snapshot` (game entity state) and `Text`,
because compression is worth wildly different amounts per traffic type:

| payload | 64 B | 1 KiB | 8 KiB |
| --- | ---: | ---: | ---: |
| binary | 0.88x | 0.99x | 1.00x |
| snapshot | 0.88x | 1.41x | 1.51x |
| text | 0.88x | 2.93x | 4.08x |

Nothing compresses below roughly 96-128 bytes; the frame header makes every kind
larger. That is what `CommonOptions::compression_threshold` (default 128) is
for.

Payloads in the comparison tables are incompressible, so those columns describe
transports rather than compression. What compression is worth by traffic type is
the ratio table above.

## Reading the numbers

These are not apples-to-apples, and the differences matter more than the
ranking. Before comparing any two rows, check which of these applies.

**znet encrypts and compresses every packet by default**, via the existing
`EncryptionLayer` plus zstd. ENet and RakNet here send plaintext and do not
compress; only GNS is comparable on that axis, encrypting with AES-GCM. So
`znet-bench` reports two profiles: `znet` as shipped, and `znet-raw` with both
off, which is the like-for-like row against ENet and RakNet.

The switch is an ordinary option, not benchmark-only scaffolding:

```cpp
ServerConfig config{"0.0.0.0", 25000};
config.child_options.common.encryption = false;
config.child_options.common.compression = CompressionType::None;
```

Both are **server-side only**. The server picks and announces its choice during
the handshake and the client adopts it, so clients need no matching config.
Setting either in `ClientConfig::options` does nothing.

**Compression runs before encryption**, so it applies to the plaintext and is
effective on encrypted and unencrypted sessions alike.

**Threading models differ, and this dominates the latency column.**

- ENet has no threads at all; the application drives it. The benchmark services
  both hosts in a tight spin loop, which is why its latency is essentially the
  raw UDP round trip. A real ENet application servicing at frame rate would see
  latency bounded by its frame interval instead. Its latency row is therefore
  not comparable with the polled libraries'.
- RakNet, GNS and znet own their threads and are polled. Their latency floors
  reflect internal update intervals, not the wire.
- znet's session workers tick at 120 Hz, but a backend with its own receive
  thread wakes them on arrival rather than letting them sleep out the tick, so a
  ZDT round trip is not bound to the tick interval. TCP reads on the worker
  itself and still is, which is why the two znet transports differ by three
  orders of magnitude there. `Server::SetTicksPerSecond()` changes the tick.

**Message rate is not byte rate.** A library that packs several small messages
into one datagram can report a message rate far above the raw UDP datagram
rate. That is real work, but it is measuring aggregation, not the socket. Check
the MiB/s column and the raw UDP floor together before concluding anything from
a msg/s number.

**GNS is still rate-limited here, and its rows are a lower bound.** Its byte
rate barely moves with payload size - 67, 96 and 91 MiB/s at 64 B, 1 KiB and
8 KiB - which is what a send-rate cap looks like, not what protocol cost looks
like. znet's rows over the same three sizes go 82, 194 and 333 MB/s, scaling the
way amortised per-message overhead should. Two GNS limiters are already raised
in `gns_bench.cc`: the ~256 KB/s default send rate, and `SendBufferSize`, whose
512 KB default was worth a further 16-31% when it was found. Whatever still caps
it has not been identified. Until the byte rate stops being flat, do not read
the 1 KiB and 8 KiB comparisons as a protocol result.

Its latency tail is its own, though, not an artifact of the setup. The
benchmark sets `NagleTime=0` so GNS sends immediately, as ZDT does; left at its
5 ms default it reports a ~10 ms round trip that measures Nagle rather than the
protocol.

It is also not doing the same work. GNS estimates bandwidth and paces its
sends where ZDT's controller only reads queueing delay, and GNS carries a
reliable byte stream rather than discrete messages, with NAT traversal, relay
fallback and certificate auth that nothing here exercises.

## Known gaps

- RakNet's 8 KiB throughput case intermittently stalls at 4999 of 5000 delivered
  and runs into the 60 s deadline, reporting ~83 msg/s. Successful runs land
  near 62,000-67,000. Re-run before trusting that row.
- The 64 B column needs several runs to mean anything. Scheduler noise
  dominates when a message costs this little: across the seven runs behind the
  published table the znet ZDT rows spanned 1.9x, GNS 1.58x and ENet 1.09x. A
  single run there is an order of magnitude, not a number. The other columns are
  steadier, GNS 1.00x and 1.02x at 1 KiB and 8 KiB, and read as written.
- The raw TCP and raw UDP floors in the published table were measured with a
  core list straddling two L3 domains, which split them into two clusters about
  2x apart (raw TCP at 1 KiB: four runs near 255,000, three near 508,000) while
  every library row stayed inside 1.4x. Client and server were landing on
  different dies. Pinning inside one domain, as above, avoids it.

- znet TCP cannot carry the 8KB case: `ZNET_MAX_BUFFER_SIZE` framing requires a
  whole message to fit in one buffer. The row reports `unsupported` rather than
  silently skipping.
- ZDT's window is counted in datagrams, not bytes, so a half-full datagram
  costs as much of it as a full one. Bytes in flight are bounded by roughly
  `max_datagrams_in_flight * MTU` per round trip.
