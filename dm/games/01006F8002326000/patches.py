# Animal Crossing: New Horizons' own code, keyed by the NSO build ID; see README.md, Screen fill.
# sead's PerspectiveProjection matrix update (main+0x37e5968; x0 the projection, x1 the matrix) reads near at +0x98, far at +0x9c, tan(fovy / 2) at +0xac, the aspect at +0xb0
# and the offsets at +0xb4 / +0xb8 (width = 2 near tan * aspect). Every perspective projection goes through it, the game's cameras and its layouts, whoever set the aspect
# (the island's camera sets its own; the projection defaults at main+0x376d0bc build 16:9 for the others). The gate at its entry has the host turn a 16:9 aspect into the
# screen's (OBS_PROJ_ASPECT, parameter 0xb0: hle_oe.c), and the front end stretches the 16:9 picture over the screen (profile screen_fill, front_ios.m, nvn_mtl.m
# present_viewport): a wider 3D view at its own proportions, the layouts centred at theirs.
OBS = {
    'ff1d1c05670db6021c85b624a710b963': [   # 3.0.3
        (0x37e5968, 27 | 0xb0 << 8, 'bd409800', 'PerspectiveProjection matrix update entry: a 16:9 aspect at x0 + 0xb0 → the screen\'s (OBS_PROJ_ASPECT)'),
    ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md)
BINDING = [
    {'what': 'PerspectiveProjection matrix update: a 16:9 aspect → the screen\'s', 'now': 'build', 'goal': 'engine', 'purpose': 'rendering',
     'why': "the function is sead's, which other Nintendo EPD games ship: a fingerprint of it could place the gate in any of them",
     'blocked': "one build on hand (3.0.3) and no other sead game's main extracted to fingerprint the function against"},
]
