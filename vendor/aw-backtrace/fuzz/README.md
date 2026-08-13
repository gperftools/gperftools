# fuzz/ — fast-path decoder fuzzing (baseline)

`fastpath-fuzz.cc` is a libFuzzer target for `TryFastFrameInfo`
(`aw-backtrace-fastpath.h`) — the single inlined pass over
`.eh_frame_hdr`+FDE+CIE that the production unwinder tries first.

## How it works

* At startup it pulls the real `.eh_frame_hdr` + `.eh_frame` bytes
  from fuzz/data/* files (or loads them from existing ELF file)
* A startup smoke test asserts the decoder accepts the pristine blob at
  its first FDE. If your chosen binary's `.eh_frame_hdr` isn't in the exact
  shape the fast path hard-codes (`enc 01 1b 03 3b`, CIE `code_align 1` /
  `data_align -8` / `z…` aug / RA at CFA-8), this aborts immediately rather
  than fuzzing a binary that can only ever hit the reject path.
* Input layout: `[u32 pc-offset][u32 data-selector][ {u32 offset, u8
  value} … ]`. See struct FuzzData in LLVMFuzzerTestOneInput. First
  word gives us PC offset to lookup, second which of the loaded datas
  to consider. Each subsequent record specifies one mutation to apply
  to the data.
* The `FuzzXlate` accessor is threaded through `TryFastFrameInfo`'s
  `Xlate` template parameter (production uses `IdentityXlate`, which
  compiles away). FuzzXlate allocates fresh (red-zone padded) chunk of
  memory for every relevant Map request and considers each mutation in
  turn for that chunk. Should help fuzzer "steer" mutation offsets
  into relevant places.

## Run

```
# needs clang; -> fuzz/fastpath-fuzz
./fuzz/build.sh
mkdir -p fuzz/corpus
./fuzz/fastpath-fuzz -use_value_profile=1 -dict=fuzz/dwarf.dict fuzz/corpus

# or e.g.
./fuzz/fastpath-fuzz --load-elf=/usr/bin/bash -use_value_profile=1 fuzz/corpus
```

`-use_value_profile=1` matters, I think.

## Data set

There is a small set of existing .eh_frame bits from my system. Adding new is as simple as:

```
objcopy -j .eh_frame_hdr -j .eh_frame -O binary /usr/lib/x86_64-linux-gnu/libc++.so.1 fuzz/data/libc++
```

Current set of "automatically loaded" datas is hardcoded in the
fastpath-fuzz.cc, so you may want to amend it too.

## Naive version

There a trivial "naive" version of the fuzzing which simply relies on
seeds from fuzz/data and lets fuzzer just mutate raw .eh_frame
stuff. It looks a little slower at finding coverage, but perhaps could
be useful too.

Run like this:

```
./fuzz/fastpath-fuzz-naive -use_value_profile=1 -dict=fuzz/dwarf.dict fuzz/naive-corpus fuzz/data/
```
