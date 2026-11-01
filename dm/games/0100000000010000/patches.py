# Super Mario Odyssey's own code, keyed by the NSO build ID; see README.md, Screen fill.
# sead's PerspectiveProjection, as in Animal Crossing: New Horizons and Super Mario Bros. Wonder (../01006F8002326000/patches.py): its matrix update reads near at +0x98,
# tan(fovy / 2) and the aspect at +0xac / +0xb0 and the offsets at +0xb4 / +0xb8. The gate at its entry has the host turn a 16:9 aspect into the screen's (OBS_PROJ_ASPECT,
# parameter 0xb0: hle_oe.c), and the front end stretches the 16:9 picture over the screen (profile screen_fill)
OBS = {
    'b424be150a8e7d78701cbe7a439d9ebf': [   # 1.4.1
        (0x9683c0, 27 | 0xb0 << 8, 'bd409801', 'PerspectiveProjection matrix update entry: a 16:9 aspect at x0 + 0xb0 → the screen\'s (OBS_PROJ_ASPECT)'),
    ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md)
BINDING = [
    {'what': 'PerspectiveProjection matrix update: a 16:9 aspect → the screen\'s', 'now': 'build', 'goal': 'engine', 'purpose': 'rendering',
     'why': "the function is sead's, the same as in Animal Crossing: New Horizons and Super Mario Bros. Wonder: a fingerprint could place the gate in any sead game",
     'blocked': 'compiled differently in each game (no exact byte match); a fingerprint needs the instruction shape reviewed across more sead builds'},
]
