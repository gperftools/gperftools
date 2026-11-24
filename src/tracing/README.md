Malloc tracer
=============

This subsystem implements very low overhead tracing of malloc events
(around 6 nanoseconds per malloc/free pair, as measured by our
`malloc_bench` on AMD Ryzen 9 7950X). Configure gperftools with
`--enable-tracing` to build it. When tracer is configured and built,
all variants of tcmalloc have it. It will be disabled by default and
needs to be activated at run-time by running program with
`TCMALLOC_TRACE_OUTPUT` environment variable pointing to file (or fifo
aka named pipe) to receive the dump. Consider also building with
`--enable-trace-compression` (needs `liblz4-dev` installed on your
system).

A few things to keep in mind:

* With `--enable-trace-compression` the trace is lz4 (frame format)
  compressed. Set `TCMALLOC_TRACE_UNCOMPRESSED=1` to get raw trace
  instead.
* Like with heap and CPU profiles, exec-ed children inherit
  `TCMALLOC_TRACE_OUTPUT` and write their traces to separate files
  with their pids appended (e.g. `trace_12345`).
* The trace is complete only if the program exits normally (via
  `exit` or returning from `main`). If it exits via `_exit` or gets
  killed, the most recent part of the trace is lost.

Traces are postprocessed and replayed by tools in
<https://github.com/alk/malloc-trace-replay>.

This tracer is described in this original design document
<https://docs.google.com/document/d/1S8T9apA2juEC5sV2UVXUan4e4_029fTmoAEoX7MEB9s/edit?usp=sharing>

Unlike other tracers it doesn't overly serialize or slow down
execution of memory allocator.

The main selling point is its overhead is low enough to be able to run
it in production (still on one-off basis, no need to spend serving
capacity on it, but it is fast enough so you can run it on one replica
of many on real traffic or do occasional run of end-user program with
it activated, and the user experience will not be degraded). Trace
fidelity is also quite high (we cannot afford to timestamp every
event, but we timestamp and note current cpu often enough).

Encoding
--------

We use variant of varint encoding described in `altvarint_codec.h`
file. Events are encoded as 1, 2 or 3 varint "words". See
`malloc_trace_encoder.h` for specific low-level encoding details.

Actual malloc and free events are saved in a per-thread buffer and
dumped wrapped in `Buf` events with timestamp, last cpu and thread id
noted as well.

Malloc events don't save addresses, but instead use implicit token
numbers. Thread grabs 1k of tokens, saving that as a `Tok` event. Then
next 1k of mallocs in that thread spend those tokens without
specifically encoding them (e.g. malloc tokens are implicit). Because
most allocations are small and sizes repeat too, we also encode size
delta from last malloc event instead of actual size. Note that tokens
are never reused.

Free events record their allocation's token (as a delta from the
previous freed token).

Note, that because events are saved per-thread and dumped in chunks,
the trace may have free event before its corresponding malloc
event. So trace reader needs logic to reconstruct the order. It is
still good tradeoff, because we're trading off encoding efficiency to
some work and complexity for offline trace analysis.

Implementation
--------------

Tracer is implemented as tcmalloc overlay (`src/alloc_overlays.h` for
concept). This lets the tracer "hook" into allocator and prepend a
small header (token and offset to the start of the underlying
allocation) to the actual allocation. So we spend extra 16 bytes per
allocation for that (or `alignment` bytes for aligned allocations with
alignment above 16, so that the returned pointer stays aligned). See
`tracing_overlay.cc` for that part. It wraps allocator `Malloc`/`Free`
operations, invokes per-thread `MallocTracer` for actual trace event
saving and token allocation and handles storing of token in the extra
space allocated for each allocation.

`MallocTracer` is a per-thread tracing layer. See
`malloc_tracer.{cc,h}`. It manages buffer, TLS details (including TLS
destructor) and actual encoding of specific malloc/free events into
the buffer. It also runs periodic dumper thread which ensures that
all the locally (per-thread) saved data is dumped into trace at least
every 3 milliseconds.

`tracer_buffer.{cc,h}` is the part that handles compressing and
writing of the data to `TCMALLOC_TRACE_OUTPUT`.

`malloc_trace_encoder.h` and `altvarint_codec.h` implement low-level
encoding details.
