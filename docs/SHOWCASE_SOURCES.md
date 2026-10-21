# README showcase sources

Catalog and feature audit: **21 October 2026**. The README summarizes
implemented behavior from each project's public documentation and release
notes. Its game list is a discovery index, not a claim that all titles or
all feature combinations have completed a campaign validation pass.

## Screenshots

These are real game captures. The showcase adds layout and labels; it does
not synthesize scenery, characters or translated text.

| Local asset | Source |
|---|---|
| `mega-man-x-coop.png` | [Mega Man X screenshot gallery](https://github.com/mstan/MegaManXSNESRecomp/blob/main/docs/screenshots/coop-thunder-slimer.png), also used in the [gameplay preview](https://1379.tech/megaman-x-recompiled-coop-zero-weapons-wip/). X and Zero fighting Thunder Slimer. |
| `f-zero-ultrawide.png` | [F-Zero Forest III capture](https://github.com/mstan/FZeroSNESRecomp/blob/main/docs/screenshots/bs-forest-iii-race-21x9.png), using optional BS F-Zero Deluxe content at 21:9. |
| `super-metroid-ultrawide.png` | [Super Metroid landing site](https://github.com/mstan/SuperMetroidRecomp/blob/main/docs/images/widescreen-landing-site.png), custom renderer. |
| `star-fox-enhanced.png` | [Star Fox Corneria capture](https://github.com/mstan/StarFoxSNESRecomp/blob/main/docs/images/starfox-enhanced-21-9-corneria.png), 21:9 with the original Arwing. The renderer is adapted from [kandowontu/starfox-enhanced](https://github.com/kandowontu/starfox-enhanced); that project remains the recommended full Star Fox port. |
| `gundam-spanish-korean.png`, `gundam-ten-translations.png` | Completed `pre_stage_battle_dialogue_3_<language>_v2` captures from the localization validation set used for [Endless Duel PR #3](https://github.com/TechnicallyComputers/GundamWingEndlessDuelSNESRecomp/pull/3). The source [contact sheet](https://github.com/TechnicallyComputers/GundamWingEndlessDuelSNESRecomp/blob/bc8554d90082b97bc1b78fcb9a3e4edc5d8f46fd/docs/assets/localization/dialogue_contact_sheet_11_languages.png) contains this scene as its second row. The new layout uses the original full-resolution captures, labels the languages and scales with nearest-neighbor sampling. |

The existing SMW and ALttP screenshots remain from the earlier README gallery.
They illustrate view expansion; they do not document every current mod or
every later renderer revision.

## Spanish and localization scope

The first row of the old Endless Duel contact sheet shows Spanish before its
second line has typed. [PR #6](https://github.com/TechnicallyComputers/GundamWingEndlessDuelSNESRecomp/pull/6)
documents the capture artifact and separately repairs ten imported Spanish
dialogue rows. The replacement showcase uses the completed Epyon dialogue:
`Asi que mi Epyon ha / sido vencido.` It preserves the game's rendered text,
including the reference font's spelling/diacritic limits, rather than drawing
replacement text over the screenshot.

Ten translated languages plus the original Japanese are available. Thai is
partial; the README does not claim that every text surface is translated or
that every language has received native-speaker review. Victory-transition
tile artifacts and some glyph limits are recorded in the localization PR.

The [localization validation notes](https://github.com/TechnicallyComputers/GundamWingEndlessDuelSNESRecomp/blob/main/tools/validation_states/README.md)
explain the pre-scene states, staged dialogue and why typewriter text must be
allowed to finish before capture. No ROM, save state or memory dump is included
in this documentation change.

## Keeping the catalog current

Check the public repository and current release notes before changing a
status. A newer release can supersede an older README claim; an open feature
PR is not a shipped feature. Preserve platform and ROM-revision qualifications.
Link source-only projects to their repository instead of inventing a release
download. Keep development targets without public downloads separate.
