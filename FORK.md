# mylonics/blackmagic - unofficial fork

This is an unofficial fork of [Black Magic Debug](https://codeberg.org/blackmagic-debug/blackmagic),
maintained for [myl_blackmagic_zephyr](https://github.com/mylonics/myl_blackmagic_zephyr).

**It is not the upstream project and contains AI-assisted changes.** Upstream
forbids AI-generated contributions, so nothing here is submitted upstream, and
issues found here must not be reported to the upstream project.

## Branches

- `main`: mirror of upstream `main` (Codeberg), advanced only by merging it. It
  starts from the last real GitHub commit of the old repo; the final
  "We have moved to codeberg" commit (which deleted the tree) lives on `archive`.
- `myl/main`: `main` plus our patch stack. West pins tags `myl-<upstream describe>-<n>`.
- `myl/rp-fixes-2026-07`: backup of the original RP2040/RP2350 work.

## Patches

Each patch is one commit whose message says why it exists and when it can be
dropped. List them with `git log --oneline main..myl/main`.
Patches are limited to `src/target`; everything else is kept byte-identical to
upstream.

## Licence

Black Magic Debug is GPL-3.0-or-later (some files BSD-3-Clause/MIT, see COPYING*).
This file and the commit history are the notice of modification.
