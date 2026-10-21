# Bounded diagnostic writers

`diagnostics/trace_writer.h` queues complete CSV rows into a fixed 4 MiB byte
ring. Its worker owns buffering, flushing, file rotation and retention. One
producer may use a writer; `Close` drains and joins outside the simulation loop.

`diagnostics/replay_writer.h` queues replay commands into a fixed 16 MiB byte
ring. The worker owns input/checkpoint files, sidecar copies, retention removals
and FNV-1a hashes of **owned copies** of the four caller-selected frame regions.
The caller still serializes a machine checkpoint at a safe simulation boundary.
The backend never reads live emulated memory from another thread.

Both queues use SDL acquire/release atomics and work with SDL2 or SDL3. A full
queue or failed disk operation stops that trace with an explicit error; neither
producer waits for the filesystem, grows a backlog, or silently drops a row and
continues claiming a complete recording. A full queue drains the accepted
prefix. An I/O failure may leave an incomplete final record, which a replay
reader must reject. The caller must check errors during recording and shutdown.
Do not close/destroy concurrently with a producer or from a frame callback.

Replay flushes run on the worker every 100 ms. A hung simulation can therefore
leave its final queued input on disk. An abrupt process kill/power loss can lose
the unflushed tail; synchronous per-input durability is deliberately not part
of the gameplay contract. Normal shutdown drains accepted records.

The session-oriented reproduction workflow follows Mega Man X4's
[Playtest Diagnostics introduced in PR #19](https://github.com/mstan/MegaManX4Recomp/pull/19)
([recorder](https://github.com/mstan/MegaManX4Recomp/blob/e83f98a9f0303265866f3eff06883abcdcb3d5bf/src/mods/mmx4_diagnostics.cpp)).
That implementation is attributed to Matthew Stanley (`mstan`) in its commit
history. These bounded asynchronous C backends are a new implementation; they
do not copy X4's game-specific recorder. Preserve upstream attribution when
adapting additional code.

Build the standalone tests, choosing either SDL backend:

```sh
cmake -S runner/tests/diagnostics -B build-diagnostic-tests -DSNESRECOMP_SDL_BACKEND=SDL3
cmake --build build-diagnostic-tests
ctest --test-dir build-diagnostic-tests --output-on-failure
```

The tests verify byte-ring wrap, ordered binary/text output, immutable frame
hashes, complete sidecars, rotation/retention, filesystem failure and a blocked
worker. The blocked worker cannot be released by production code: the test
asserts that producers return and stop at capacity before releasing it.
