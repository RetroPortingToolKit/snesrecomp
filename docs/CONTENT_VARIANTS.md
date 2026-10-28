# Program-module and content-selector internals

This is a maintainer reference for the existing module registry, selector,
and save namespaces. For mod authoring, use [`MOD_AUTHORING.md`](MOD_AUTHORING.md).

**ROM patching is forbidden.** IPS/BPS application, recompilation of patched
ROMs and runtime writes into the cartridge image are unsupported. Legacy
`[[patch]]` sections and variant `patch` fields are rejected. Use trusted mod
hooks and semantic content loaders against stock code instead.

A **content variant** identifies a program selection inside an executable.
Its **program module** supplies the compiled dispatch tables and image
identity. The registry associates a selection with its ROM source, boot
policy, safe selector screen, and isolated saves. These facilities do not
make arbitrary patched ROMs supported mods.

```
        executable
        ├── stock program module        (src/gen, unprefixed)        -> variant "smw"
        └── program module "smas"       (src/gen-smas, prefix smas_) -> variants smas.smb1, smas.smbll, ...
```

## Program modules (`runner/src/program_module.h`)

One generated tree = one module. `v2_emit` now emits `module_v2.c`, whose
constructor registers the tree's dispatch table, RAM-routine guards and the
SHA-256/size of the image it was generated from:

```
python snesrecomp/tools/v2_emit.py --rom smas.sfc --cfg-dir recomp/smas \
    --out-dir src/gen-smas --module-id smas --module-prefix smas --cfg-roots
```

`--module-prefix` makes two modules linkable into one executable: the emitter
writes `module_namespace.h` (`#define <sym> smas_<sym>` for every generated
definition, derived from the emitted text itself, not a regex run afterwards),
includes it first in every translation unit, and copies the module's own
`funcs.h` beside them so `#include "funcs.h"` resolves to that module's
declarations. A tree generated without a prefix keeps today's symbol names, so
existing ports change nothing.

The host links an extra module with:

```cmake
snesrecomp_target_program_module(<target> ${CMAKE_SOURCE_DIR}/src/gen-smas OPTIONAL)
```

`OPTIONAL` lets the title build without the tree (the runtime then reports
"module 'smas' is not part of this build" on that variant). Without it an empty
directory fails the configure, like `snesrecomp_target_generated_code`.

Selecting a module is `snes_program_module_select()` = `cpu_select_program()`
with that module's tables; it must happen before guest execution, i.e. across a
machine rebuild.

## Variants (`runner/src/content_variant.h`)

The port registers its stock variant once:

```c
snes_variant_register_builtin("smw", "Super Mario World", "main",
                              "mario", /*order*/0,
                              /*selector screen*/1, 0x0100, 0x08);
snes_variant_bind_game_info("main", &kSmwGameInfo);
```

The existing format-1 registry can also describe an unmodified external
program in a title that already integrates it. This fragment documents that
integration contract, not a general mod-author workflow:

```toml
[[external_rom]]
feature = "smas"
id = "smas-usa"
label = "Super Mario All-Stars (USA)"
format = "snes"
size = 2097152
sha256 = "..."              # verified by the engine before any use

[[variant]]
feature = "smas"
id = "smas.smb1"
display_name = "Super Mario Bros."
module = "smas"             # program module id
source_rom = "smas-usa"     # external_rom id; omit for the launched ROM
save_namespace = "smas.smb1"   # default: id
selector_group = "mario"    # variants sharing the stock variant's picker
selector_order = 10
selector_wram = "0x0100=0x08"  # screen on which this variant shows the picker
boot_pokes = "0x7FFF00=0x02"   # generic frame driver: pokes before first run
boot_entry = 0x008000          # optional entry PC instead of the reset vector
```

A variant is **available** only when: its feature is enabled in the committed
plan, its module is linked, and its source ROM is selected and hashes as
declared. The runtime additionally checks that the image matches the module's
own recorded digest. A wrong ROM never boots; the picker shows the reason
instead.

### Frame drivers

A port's own `RtlGameInfo` encodes what "one frame" means for *that* title. A
variant running a different program uses the engine's generic vector-driven
LLE driver (`generic_frame_driver.c`, the scaffold template lifted into the
engine) unless the port binds a driver to that module id. The boot policy
(`snes_boot_policy.h`) is applied on the first frame after the reset vector is
read: WRAM/bus pokes and an optional entry PC.

### Saves

`saves/` stays the stock variant's profile 1 so nothing changes for an existing
player. Every other (variant, profile) lives at
`saves/variants/<save_namespace>/p<N>/` with the usual `save.srm`,
`save<K>.sav`. `RtlWriteSram` is now write-temp, flush, rotate `.bak`, rename,
because a switch flushes the outgoing namespace at the moment it matters most.
The last selection is remembered in `saves/variants/last-selection.txt`.

### Switching

Host-driven, on the port's existing session-reboot path:

```
RtlWriteSram()                      // old namespace
snes_free()
snes_variant_prepare(v, profile, launched_rom, size, &rom, &rom_size, &owned, err, cap)
    // verify -> module select -> RtlRegisterGame(driver) -> save root
SnesInit(rom, rom_size)
RtlReadSram()                       // new namespace
snes_variant_selector_sync()
```

## Picker (`runner/src/variant_selector.h`)

Framework-owned like `snes_savestate_menu`: the host filters the pad word
before `RtlRunFrame`, polls for a confirmed switch after it, and blits the
overlay into the presented frame. Left/Right browse the group; while the
highlight is away from the active variant, Up/Down choose that variant's
profile and the D-pad/face buttons are masked from the guest; A/Start confirm,
B/X/Y return. With a single registered variant the picker never draws and
never masks: a title with no variants installed is bit-identical to a build
without it.

## Tests

- `tests/test_program_module_emit.py` -- descriptor, namespace, identity rules.
- `tests/program_module/`, `tests/content_variant/` -- C
  unit tests (registry, IPS/BPS with every checksum, save roots + selector
  state machine), all in `tests/run_c_tests.sh` and each with a `run.ps1`.
