# Making a `.snesmod` by hand

You can make SNESRecomp mods with a text editor, ordinary programming tools,
and a ZIP utility. No AI service, account, or generated artwork is required.
This guide explains the format, walks through a first package, and shows where
to add your own behavior.

The file extension is **`.snesmod`** (singular). It is a ZIP containing a
`manifest.toml` at its root. The manifest describes the mod and the switches
the launcher should show. The archive carries data; game-specific native code
is compiled into the game's executable separately.

Game releases pin particular framework versions. Check the target game's
source and release notes: a feature in this framework checkout may require a
newer game build than the one you have installed.

- [Choose a starting point](#choose-a-starting-point)
- [Make your first package](#make-your-first-package)
- [Add options and data](#add-options-and-data)
- [Package and install it](#package-and-install-it)
- [Reimplement an existing ROM hack](#reimplement-an-existing-rom-hack)
- [Test and share](#test-and-share)
- [Troubleshooting](#troubleshooting)

## Choose a starting point

| What you want to make | Work involved |
| --- | --- |
| A package using an existing game plugin | Write a manifest and any data that plugin expects. No game rebuild if the released executable already contains the implementation. |
| New gameplay, a new renderer, or a new audio feature | Write C/C++ in the game repository, register a trusted plugin, rebuild the game, and supply a manifest selecting it. |
| A translation | Use the game's existing localization integration and table format, or add one. See [Runtime Localization](RUNTIME_LOCALIZATION.md). |
| A mod inspired by an existing ROM hack | Study its changes and reimplement the behavior and supported data as a mod against the stock ROM. Do not recompile the patched ROM. |

An invented plugin ID does not create an implementation. Likewise, putting
source code, a DLL, an IPS file, or an image in the archive does not make the
game execute or use it automatically.

The useful terms are:

- **Package:** the thing you install and version, such as `yourname.practice`.
- **Feature:** one independently enabled switch inside that package.
- **Option:** a value belonging to a feature, such as a difficulty choice.
- **Plugin:** C/C++ behavior already linked into the game, selected by an ID.
- **Resource:** a file or folder the player chooses through the launcher.

At launch, the runtime checks the ROM and enabled features, checks plugin
availability and duplicate claims, saves the choices, resets mod-controlled
state, and activates the selected plugins. Changes generally take effect on
the next game launch.

## Make your first package

This tutorial makes a small startup-message mod. It deliberately starts with
something whose activation you can see in the game log before you attempt
gameplay changes. **It requires rebuilding the target game once.** If you are
only packaging an existing plugin, use that plugin's documented package,
feature, option, and asset names instead of the tutorial names.

### 1. Gather the target information

You need:

- The game repository and its working build instructions, for a new plugin.
- A supported stock ROM supplied by you.
- Python **3.11 or newer** for the syntax-check commands below.
- A text editor that saves UTF-8 **without a byte-order mark**.

Find the exact `game_id` and `rom_sha256` in the game's existing
`mods/preloaded/packages/*/*/manifest.toml` files or in its call to
`snes_mod_runtime_initialize_c()`. Use the IDs from the source, including case
and punctuation. The SNES loader requires an exact game ID and a lowercase
64-character ROM hash; `game_id = "*"` is not a SNES wildcard.

The ROM hash is over the cartridge bytes, with a 512-byte copier header
removed when the file size indicates one. You can calculate it without
changing the file:

```sh
python -c "from pathlib import Path; import hashlib; b=Path('game.sfc').read_bytes(); b=b[512:] if len(b)%32768==512 else b; print(hashlib.sha256(b).hexdigest())"
```

The result must agree with a revision the game actually supports. Changing a
manifest hash does not add support for another ROM revision.

### 2. Write `manifest.toml`

Create a folder named `my-first-mod`, and save this as its `manifest.toml`.
Replace the two capitalized target placeholders with the values above before
installing. The rest of the example matches the C code in the next step.

```toml
format_version = 1
id = "tutorial.hello"
version = "1.0.0"
name = "My First SNES Mod"
author = "Your Name"
description = "Prints a message when this feature is activated."
license = "MIT"
resolver = "declarative"
save_compatibility = "shared"

[[target]]
game_id = "REPLACE_WITH_GAME_ID"
rom_sha256 = "REPLACE_WITH_ROM_SHA256"

[[feature]]
id = "hello"
name = "Startup message"
description = "Print a startup message in the game log."
group = "Tutorial"
default_enabled = false

[[plugin]]
feature = "hello"
id = "tutorial.hello.startup"
```

Use your own package namespace for published mods. Keep package IDs stable
across updates, and increase `version` when distributing changed contents.
Use lowercase letters, digits, dots, hyphens, and underscores in IDs. Display
names can contain spaces and normal capitalization. Choose the license for
your own work; the example's `MIT` does not license somebody else's assets.

`format_version` describes the manifest schema; `version` describes your mod.
`save_compatibility` is metadata, not an automatic migration of existing saves.
Test your mod's actual save behavior.

### 3. Register the behavior in the game

In the **game repository**, create `src/hello_mod.c`:

```c
#include <stdio.h>
#include "mod_runtime.h"

static void hello_activate(void) {
    fprintf(stdout, "My First SNES Mod is active.\n");
    fflush(stdout);
}

SNES_MOD_CONSTRUCTOR(register_hello_mod) {
    (void)snes_mod_register_activation_plugin(
        "tutorial.hello.startup", hello_activate);
}
```

Add the file to the game's existing executable target, replacing `MyGame`
with its actual CMake target name:

```cmake
target_sources(MyGame PRIVATE src/hello_mod.c)
```

Follow that game's normal configure/build instructions. Do not put this C
file in the `.snesmod` expecting the installer to compile it. Players need
your rebuilt executable, or an upstream release that includes the plugin.

For a game that has not integrated mods yet, the maintainer must enable
`SNESRECOMP_ENABLE_MODS`, initialize and commit the runtime, supply its launcher
provider, and activate the committed plugins. The framework's
[integration guide](MOD_PACKAGES.md) and scaffolded game host cover that
wiring. Merely adding a package does not enable the Mods page.

### 4. Develop the actual change

Once the startup message works, replace its body with the behavior you want.
For gameplay work, identify the relevant state in the game's disassembly,
symbols, and debugger first. Record the supported revision, byte widths,
addresses, and when those values are valid. A CPU bus address and a ROM file
offset are different coordinates.

For behavior that runs each emulated frame, register a callback **during
activation** with `snes_mod_register_frame_callback()`. The runtime clears
frame callbacks when activating a new plan. Registering one unconditionally
in the constructor would bypass the feature switch.

For controller remaps and button macros, register an input transform during
activation with `snes_mod_register_input_transform()` (`mod_runtime.h`). The
desktop host applies it to each local seat's 12-bit button word before netplay
exchange, replay recording, run-ahead and the guest see it, so it stays
deterministic without being part of the simulation. It may read guest WRAM and
the host's right stick and analog triggers (`SnesModInputContext`); it must not
write WRAM. Behaviour that has to change the simulation itself belongs in a
[`mod_hook`](MOD_HOOK.md).

If a plugin sets a persistent host flag, register a reset callback with
`snes_mod_register_reset_callback()` to restore the stock value before enabled
plugins activate. Check enabled-to-disabled behavior in the same executable.
Keep registration and actual gameplay effects separate: registration makes
code available; activation makes it run.

To change what the game's own code does at a specific instruction (skip a
check, adjust a value before it is used, return early), declare a
[`mod_hook`](MOD_HOOK.md) in the port's bank cfg and arm it from activation
with `snes_mod_hook_arm()`; disarm it in the reset callback. The hook runs in
compiled and interpreted code alike, so the routine stays compiled.

## Add options and data

An option gives the launcher a control. Your plugin must read and implement
it. For example, add this after the feature declaration in the tutorial:

```toml
[[option]]
feature = "hello"
id = "verbosity"
label = "Message style"
type = "choice"
default = "short"

[[option.choice]]
value = "short"
label = "Short"

[[option.choice]]
value = "detailed"
label = "Detailed"
```

Read it from the activation callback using
`snes_mod_runtime_feature_option_value_c("tutorial.hello", "hello",
"verbosity", buffer, sizeof buffer)`. It returns text in your buffer and a
success flag. Compare against the declared values and supply a sensible
fallback if the lookup fails. `boolean` and bounded `integer` options are
also supported; integers declare `min`, `max`, `step`, and `default`.

Existing plugins may hard-code their package, feature, and option IDs in these
queries. Renaming those IDs while reusing a plugin can break its settings.
Follow the implementation's contract.

Files shipped with the package can live under `assets/` or another relative
directory. A plugin locates its selected package with
`snes_mod_runtime_package_root_c()` and reads the files it understands there.
There is no generic rule that an `assets/` folder overrides ROM graphics.

For something the player owns separately, declare a picker instead:

```toml
[[resource]]
feature = "hello"
id = "artwork"
label = "Your artwork"
format = "file"
file_patterns = "*.png"
file_description = "PNG images"
required = false
```

This example adds a picker only; the startup-message plugin does not draw
images. A real consumer retrieves the selection with
`snes_mod_runtime_resource_path_c()`. A required resource blocks an enabled
feature until a suitable path is selected.

## Package and install it

Your source folder should contain only what you intend to distribute:

```text
my-first-mod/
  manifest.toml
  README.txt
  assets/          (only if your plugin needs packaged data)
```

Write a short `README.txt` describing the target game build and how to enable
the feature. From inside `my-first-mod`, check the TOML and make the archive:

```sh
python -c "import pathlib,tomllib; tomllib.loads(pathlib.Path('manifest.toml').read_text(encoding='utf-8')); print('TOML syntax OK')"
python -m zipfile -c ../my-first-mod-1.0.0.snesmod manifest.toml README.txt
python -m zipfile -l ../my-first-mod-1.0.0.snesmod
python -m zipfile -t ../my-first-mod-1.0.0.snesmod
```

If you have assets, append `assets` to the archive-creation command. Keep the
output archive **outside** the source folder. The listing must show
`manifest.toml`, not `my-first-mod/manifest.toml`. The TOML check catches syntax
errors; the actual loader checks supported fields, targets, and plugins.

You can also use a graphical ZIP utility: select the **contents** of the
folder, create a ZIP, and rename its extension to `.snesmod`. Enable visible
file extensions so it does not become `.snesmod.zip`. Use ordinary ZIP storage
or DEFLATE compression, without encryption. The installer limits archives to
4096 entries and 256 MiB expanded.

Open the target game's launcher, go to **Mods**, use its package installation
control, and choose the archive. Enable **Startup message**, then launch the
game and check its console/log. Disable it and launch again: the message must
stop appearing. The exact management button labels depend on the pinned UI.

Current scaffolded SNES games use the following root beside the executable:

```text
mods/preloaded/
  state.toml                       player selections; do not distribute
  packages/tutorial.hello/1.0.0/
    manifest.toml
    README.txt
```

Older games may use `mods/` as the root. Check `SNES_MOD_CATALOG_ROOT` and the
title's runtime initialization instead of guessing. Installing the archive
through the launcher chooses the correct location. During game development,
bundled source packages belong under
`mods/preloaded/packages/<id>/<version>/` and are staged with
`snesrecomp_target_mod_catalog()`; avoid editing a build's staged files.

## Reimplement an existing ROM hack

ROM patching is forbidden: do not apply IPS/BPS, recompile patched ROMs, or
write into the loaded cartridge image. Legacy package `[[patch]]` sections
are rejected. Use declared mod hooks/operands, input transforms, presentation
overlays and validated semantic content loaders.

For a configurable immediate use [`mod_operand`](MOD_OPERAND.md); it stores
the mod's value outside the cartridge and applies it in both execution tiers.

**Do not recompile ROM hacks.** The recommended approach is to understand the
hack's changes and implement them as a `.snesmod` using the supported stock
ROM and the game's mod integration. Players keep using their stock ROM.

An IPS/BPS file is useful evidence of changed bytes, but it does not explain
the purpose of those changes. A large byte diff can mix relocated code,
compressed graphics, pointers, level data, and gameplay edits. Start with the
hack's source and documentation when available, then use disassembly,
debugger observations, and comparisons to understand each intended change.

1. List the actual features you want to reproduce: for example, a different
   starting loadout, new character behavior, revised levels, or new graphics.
2. Separate behavior from data. Implement behavior in the game's trusted
   plugin and use package assets in the formats that implementation consumes.
   Add the necessary data-loading support if the game does not have it yet.
3. Connect each feature to an explicit manifest switch and its options. Keep
   unrelated enhancements independently selectable.
4. Verify the expected stock state before making revision-sensitive changes.
   Implement new logic through the game's mod interfaces; copying injected
   65816 code into ROM bytes does not make it part of the compiled game.
5. Compare the mod's behavior with the original hack as a reference. Test
   actual mechanics, stage transitions, saves, and feature combinations.
6. Credit the original authors and obtain permission for redistributed code
   or assets where required. Explain which features you reproduced and which
   remain unsupported.

This can require hand-written C/C++ and a game build that includes the new
plugin. That build adds a mod implementation to the stock game's executable;
the workflow does not analyze and recompile a patched ROM. Once the game
ships the implementation, the `.snesmod` selects it and supplies its data.

## Test and share

Test the archive you will give players, using a separate game installation and
fresh mod selections. Keep your normal saves backed up.

- Confirm the intended stock revision works and unsupported revisions are
  rejected. Record the exact game build you used.
- Check enabled, disabled, and enabled-to-disabled launches. Try every option,
  including integer limits, and missing resources where applicable.
- Exercise the mechanics you changed: gameplay interactions, transitions,
  death/retry, sound, and save/load as relevant. A visible Mods row proves
  discovery, not that the behavior works.
- Try relevant combinations with other mods. Features claiming the same
  trusted plugin are alternatives: selecting one deselects the other. A stale
  saved selection with two enabled claimants is rejected on Play.
- For netplay claims, validate actual sessions with the supported builds and
  matching mod versions/settings. `presentation_only = true` alone does not
  grant an exemption from mod agreement.

Share the `.snesmod`, your source, license/credits, build requirements, and a
short changelog. Clearly state when the mod requires a custom executable.
Include only data you have permission to redistribute; player-selected ROMs
and personal `state.toml` selections do not belong in the archive.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| No Mods page | The game must enable mods and supply the runtime's launcher provider. |
| No root manifest | ZIP the folder's contents, with `manifest.toml` at archive root. |
| Invalid/unsupported field | SNES currently uses format 1 and a restricted parser. Use the shown one-line fields and double-quoted strings; PSX patch/condition syntax does not transfer. |
| Invalid target / wrong game | Use the exact game ID and header-normalized lowercase ROM SHA-256. |
| Missing trusted plugin | Install a game build containing it, or add/register/link the implementation. |
| Two features claim a plugin | Selecting one alternative deselects the other. If an old saved selection still has both enabled, disable one; package order does not pick a winner. |
| Option changes do nothing | The implementation must query that exact package/feature/option identity. |
| Already installed | Bump the mod version, or remove your previous test version through the manager before reinstalling. |
| An IPS/BPS file has no effect | Reimplement the hack's features through the game's mod integration; the archive is not an automatic ROM-hack converter. |

For exact APIs and implementation details, see
[`runner/src/mod_runtime.h`](../runner/src/mod_runtime.h),
[`runner/src/mod_runtime.cpp`](../runner/src/mod_runtime.cpp), and
[`docs/MOD_PACKAGES.md`](MOD_PACKAGES.md).
