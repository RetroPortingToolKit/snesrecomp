# Host audio producer backpressure

Guest time remains authoritative for SPC execution. A fast producer can still
fill the native PCM FIFO during a long loader or multi-frame guest operation.
Backpressure lets an active realtime device consume that PCM before the
producer continues, without advancing another emulation clock or trimming PCM.

The desktop host previously waited in `RtlApuUnlock`, releasing the mutex only
around each sleep. Its stall latch was rearmed by any audio callback, including
callbacks that did not consume native samples. Such callbacks could add another
250 ms timeout to each subsequent producer checkpoint.

The desktop now registers `HostWaitForAudioProducer` through the optional shared
hook and invokes it only after releasing the outermost APU mutex. It samples the
FIFO under the mutex, then sleeps outside it while occupancy exceeds 4096 native
stereo frames. A wait has a 250 ms elapsed-time budget; unsigned tick subtraction
handles SDL2 tick wrap. On timeout, subsequent checkpoints return immediately
until the native FIFO read cursor changes or occupancy falls below the threshold.
This covers repeated port writes without charging each word another stall budget.

## Integrating another host

1. During host initialization, register a callback with
   `RtlAudioSetProducerWait(wait_for_consumer)`. Pass `NULL` to unregister. Do not
   change registration concurrently with producer execution.
2. In the producer thread's `RtlApuUnlock`, release the mutex first, then call
   `RtlAudioWaitForConsumer()` when the outermost lock is released. A host may
   instead call it at its own unlocked producer checkpoints.
3. The host must gate waits on an active realtime consumer. Paused, headless,
   disabled-device, and turbo paths must return immediately. The desktop uses
   its existing thread-local `g_audio_producer_active` policy.
4. Bound waits and suppress repeated stalled waits using actual FIFO consumption,
   rather than callback notifications. Leave guest clocks and queued PCM alone.

The shared helper skips unregistered consumers, fast-forward, and speculative
frames. Its thread-local recursion guard permits a callback to inspect the FIFO
with `RtlApuLock/Unlock` without recursively invoking itself. The hook is an
explicit host integration point; the runtime does not call arbitrary host code
from inside its locked SPC operations.

## Validation and limits

`python -m pytest tests/test_audio_producer_pacing.py -q` compiles the production
functions into a synthetic ROM-free device harness. It verifies unlocked callback
entry and sleeps, nested mutex release, default opt-out, recursive inspection,
normal consumption, tick wrap, bounded stalls, 100 stalled upload checkpoints,
callback notifications without consumption, recovery after consumption, and
fast-forward/speculation/inactive-consumer bypass. It also checks that pacing
does not change the write cursor or discard samples.

The timeout measures elapsed ticks, not an operating-system scheduling guarantee.
A stopped device intentionally lets guest execution continue after the budget;
existing FIFO overflow behavior remains possible when nothing consumes PCM.
This change does not modify SPC upload handshakes, frame timing, sample-rate
conversion, or the existing fast-forward PCM trimming policy. Those behaviors
need separate regressions before any game-specific timing changes are ported.
