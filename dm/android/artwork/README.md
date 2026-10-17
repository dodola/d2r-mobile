# Launcher icon

Original transparent raster artwork generated with the built-in image_gen tool
on 2026-10-16. `d2r-icon-source.png` is the preserved master;
`d2r-icon-preview.png` is the opaque 512px legacy-icon preview.

Prompt: Original dark-fantasy Android launcher emblem: a centered, symmetrical
forged obsidian-iron horned helmet, antique bronze bevels, molten red eye slits
and an orange-red vertical fracture. Strong simple silhouette readable at 48px,
hand-painted 3D game-icon finish, transparent background, generous adaptive-icon
safe margins, no scenery, frame, words, numerals, logos or watermark; not a copy
of an existing game's icon.

`tools/d2r-mobile-port/build_launcher_icons.py` preserves the master alpha and
resamples it for deterministic Android packaging. Adaptive art fits within the
central 60% of the 108dp foreground canvas; the background matches the existing
shell ink. Legacy launchers receive five density-specific opaque tiles. The
icon is app branding only and does not alter the original game renderer or UI.
