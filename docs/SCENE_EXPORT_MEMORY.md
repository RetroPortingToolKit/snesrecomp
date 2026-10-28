# Scene export scratch lifetime

The audio-port ring retains pre-query history in every build. A scene export
copies that ring under the audio lock, then writes the snapshot to `.apu.tsv`.
Keep the ring and its recording rules intact: starting recording on the first
query loses the history the query needs.

Previously, `state_dump.c` reserved a second full-capacity static array solely
for formatting that export. It now opens the output, allocates one bounded
scratch array, takes the existing complete snapshot, formats after releasing
the audio lock, and frees the array. Repeated chunk queries would allow
producers to advance or overwrite history during formatting, so the export
still takes one complete snapshot.

Open, allocation, write and close failures report failure through the existing
scene-dump return contract. As with the exporter's other transient allocations,
an export can fail under memory pressure; no permanent fallback allocation is
retained. The allocation never occurs during ordinary frame execution.

The removed static array is 1,572,864 bytes on the measured Windows x64 ABI.
This is a BSS/reservation reduction, not a claim that every unused page was
resident. A full snapshot still needs its scratch memory while exporting.
Neither guest state nor renderer/feature defaults change.

## Qualification in progress, 2026-10-28

Owner `beads-8wg.2.168`, campaign `beads-2wy8.1`, shared R9 follow-up. Windows
and Linux contracts pass using the real retained audio ring: empty export,
full-capacity wrap/order, producer advancement during formatting, all four
failure classes, cleanup and propagation through the public scene-dump API.
Production code has no test interposition or new runtime selector.

This change stacks on [compact raster registers](RASTER_REGISTERS.md). The
combined candidate still requires matched built-game correctness/performance
and owner validation. BSS, resident/peak memory and CPU costs must be reported
separately. No original Xbox qualification is implied.
