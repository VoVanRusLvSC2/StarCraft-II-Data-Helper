# StarCraft-II-Data-Helper 3.0 Beta 3

This is a pre-release. It must not be treated as Stable or Final.

## Highlights

- Map Performance now creates a separate **Decor-Control Map Copy**: it keeps
  `Objects` byte-identical and publishes Galaxy hide/restore functions for
  safe existing doodad actors.
- The generated Galaxy import uses the Editor-safe extensionless form
  (`include "scripts/sc2dh_decor_opt"`), preventing the erroneous
  `.galaxy.galaxy` path after Editor regeneration.
- The CLI supports `--map-regions --visibility-only` to use real map Regions
  without manually making a zones JSON file.

## Safety and verification

- Source maps are never overwritten by this workflow.
- The generated copy is structurally verified, but a green internal verifier
  is not proof of Galaxy Editor acceptance.
- Real Editor acceptance and manual in-game checks remain required before any
  Stable/Final claim.

## Included artifact

- `StarCraft-II-Data-Helper-3.0-Beta-3-<commit>-win64.zip`
