# Cumulative SNES campaign comparison (2026-10-29)

The recent package reduced measured CPU cost in all three matched workloads. These are the builds immediately before the first October 22 campaign changes versus the owner-qualified final package. Original historical engine/UI pins are preserved. Earlier optimizations remain part of the baseline.

| Game | Before CPU ms/frame | After CPU ms/frame | Lower CPU cost |
| --- | ---: | ---: | ---: |
| Mega Man X | 6.089 | 1.936 | 68.2% |
| Super Metroid | 7.930 | 3.212 | 59.5% |
| Super Mario World | 6.055 | 4.941 | 18.4% |

Two samples per arm in ABBA order (before, after, after, before), with fixed frame-based warmup and matched widescreen, audio and gameplay settings. Each pair stays on one host: MMX/SMW on the 9800X3D PC, Metroid on SERVER/i5-4670. The real GUI/title rendering pipeline runs with software SDL and muted dummy output. Uncapped throughput varies more with host load; process CPU is the primary headroom measure. Gameplay pacing remains unchanged.

These are cumulative title/engine/UI packages, including intervening correctness changes. The three games did not receive identical packages. The measurements do not isolate a single optimization or establish whole-game, netplay, Xbox or other-platform performance. Both comparison orders show lower CPU for each game. MMX candidate CPU spread is 9.23%; its reductions are 66.7% and 69.7%. SMW paired reductions are 19.08% and 17.72%. Metroid CPU spread is about 1.5%.

Metroid adaptive widescreen remains supported. Its sample has camera X=0 and 43 added pixels on the left: world X=-43 through -1 lies outside the authored room. The candidate replaces 6,521 wrapped terrain pixels there with black. Native center and valid right margin RGB are identical. This correctness fix and its saved work are included in the 59.5% cumulative result; it is not an isolated optimizer comparison. MMX and SMW presented captures match exactly.

Workloads: MMX freshboot movement/shooting route, timed frames 2801 through 6000; Metroid stationary room 0x9E9F reached through the native door loader, frames 3601 through 12000; SMW running scrolling level 0x2A, frames 2601 through 3000. Captures and state observations are separate from timed runs. Owner-cleared gameplay checks were not repeated.

Machine-readable summary: [JSON](measurements/2026-10-29-cumulative.json). All 12 cost samples, source/binary/input identities and raw evidence links: [CSV](measurements/2026-10-29-cumulative-samples.csv). Full local evidence and independent audit packets remain at `G:\Temp\Codex\snesrecomp\native-hle-r8-20261029\cumulative-20261030`. Coordinating issues: `beads-8wg.2.185`, `beads-2wy8.1`.
