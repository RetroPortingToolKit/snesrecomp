/* Present a title's built-in launcher mod provider and the package runtime's
 * provider as one Mods page.
 *
 * A title may own features the package runtime does not (Super Metroid's
 * live-editable renderer settings, stored in its own file and changed from
 * the pause menu) while also shipping manifest packages for the runtime.
 * recomp-ui takes exactly one provider, and the desktop host used to pick
 * the title's and silently drop the runtime's, which hid every package.
 *
 * Each package/feature keeps exactly one owner: the provider that lists it.
 * Lists are concatenated (first, then second); calls naming a package are
 * routed to its owner. If both list the same package id the first owns it.
 * commit/commit_netplay run on both and fail if either fails. Archives are
 * installed by the second provider (the runtime) when it accepts them.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct RecompLauncherCModProvider;

/* Returns a provider valid until the next call. Either argument may be NULL,
 * in which case the other is returned unchanged. */
const struct RecompLauncherCModProvider *snes_mod_provider_compose(
    const struct RecompLauncherCModProvider *first,
    const struct RecompLauncherCModProvider *second);

#ifdef __cplusplus
}
#endif
