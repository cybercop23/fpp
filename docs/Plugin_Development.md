# Developing a Plugin for FPP

FPP's Plugin Manager reads its list from
[`FalconChristmas/fpp-data/pluginList.json`](https://github.com/FalconChristmas/fpp-data/blob/master/pluginList.json).
Each entry is `[displayName, pluginInfoURL, category]`; `pluginInfoURL` points at a
`pluginInfo.json` in the plugin's own repository, which FPP fetches directly to list,
version-check, and install the plugin.

**Everything about building a plugin lives in the
[fpp-plugin-Template](https://github.com/FalconChristmas/fpp-plugin-Template)
repository, not here:**

- [`PLUGININFO_FORMAT.md`](https://github.com/FalconChristmas/fpp-plugin-Template/blob/master/PLUGININFO_FORMAT.md)
  — the full `pluginInfo.json` field reference (required/optional fields, the
  `versions[]` array, `platforms`, resource hints, the `dependencies` block).
- [`PLUGIN_GUIDELINES.md`](https://github.com/FalconChristmas/fpp-plugin-Template/blob/master/PLUGIN_GUIDELINES.md)
  — rules and conventions for a well-behaved plugin: logging, the install/uninstall
  lifecycle, talking to FPP through its API instead of its internals, dependency
  installation, and UI conventions.
- the template plugin itself — a working skeleton to fork.

For getting your finished plugin **listed** (or later de-listed/retired) in FPP's
Plugin Manager, see the
[fpp-data README](https://github.com/FalconChristmas/fpp-data/blob/master/README.md)
— it covers the submission process, the `pluginList.json` entry format, and how to
request removal.

Keep those as the source of truth; this page only covers the two things that are
about *FPP itself* rather than about writing a plugin.

## How FPP installs a plugin

FPP `git clone`s the `srcURL` from `pluginInfo.json` into the plugin's directory,
then runs `scripts/fpp_install.sh` to set it up. On uninstall it runs
`scripts/fpp_uninstall.sh`.

## Plugin types

- **Script plugin** — PHP or bash scripts invoked by FPP commands, wired up via
  `commands/descriptions.json`. See the
  [template plugin](https://github.com/FalconChristmas/fpp-plugin-Template/) for a
  working example.
- **C++ plugin** — a shared library (`.so`) linked into the `fppd` daemon at
  runtime, subclassing the `Plugin.h` base class. See
  [fpp-brightness](https://github.com/FalconChristmas/fpp-brightness) for a working
  example.
