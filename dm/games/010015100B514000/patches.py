# Super Mario Bros. Wonder's own code, keyed by the NSO build ID; see README.md, Screen fill.
# sead's PerspectiveProjection, as in Animal Crossing: New Horizons (../01006F8002326000/patches.py): its vtable (main+0x3682cf0 in 1.2.1, found by its getters at +0x20..+0x38:
# near +0x98, far +0x9c, fovy +0xa0, aspect +0xb0) puts the matrix update at +0x58, main+0x3c0178. The gate at its entry has the host turn a 16:9 aspect into the screen's
# (OBS_PROJ_ASPECT, parameter 0xb0: hle_oe.c), and the front end stretches the 16:9 picture over the screen (profile screen_fill)
OBS = {
    'ff773e90972d544eb79406eaa65396d5': [   # 1.2.1
        (0x3c0178, 26 | 0xb0 << 8, 'bd409800', 'PerspectiveProjection matrix update entry: a 16:9 aspect at x0 + 0xb0 → the screen\'s (OBS_PROJ_ASPECT)'),
        # The courses are drawn through sead's OrthoProjection (vtable main+0x3682d18, the matrix update at +0x58: main+0xf18f0; box top / bottom / left / right at +0xa0,
        # the matrix-dirty flag at +8), a box 15.5 units high and 27.56 wide at 16:9, which the perspective gate does not reach. The camera update at main+0x119d80 (x1: the
        # projection) rebuilds the projection's matrix when it is dirty and takes from it the camera's matrix (+0x100 from x0), inverse, frustum planes and near / far / fovy /
        # aspect: the gate at its entry widens a 16:9 box to the screen's aspect and marks it dirty (OBS_PROJ_ORTHO). Not at the matrix update itself: the class also makes the
        # final composite's full-screen pass (1920 x 1080) and the layouts', which widened leave bars at the sides. Found with DM_NVN_CAPTURE / DM_NVN_CBDUMP on the course
        # shaders (cb0: the view at +0, the orthographic projection at +0x70), DM_PROJ_SCAN_PAIR and DM_PROBE_WATCH (the uniform block copied at main+0x16bf3c; the camera's
        # matrix written at main+0x119e98)
        (0x119d80, 29 | (0xa0 | 1 << 12 | 8 << 17) << 8, 'd10203ff', 'camera update entry: the OrthoProjection at x1, a 16:9 box at +0xa0 → the screen\'s width, dirty at +8 (OBS_PROJ_ORTHO)'),
    ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md)
BINDING = [
    {'what': 'PerspectiveProjection matrix update and the courses\' view-projection build: a 16:9 projection → the screen\'s', 'now': 'build', 'goal': 'engine', 'purpose': 'rendering',
     'why': "the function is sead's, the same as Animal Crossing: New Horizons' (vtable layout and the function's first instructions): a fingerprint could place the gate in any sead game",
     'blocked': 'the two games compile it differently (no exact byte match); a fingerprint needs the instruction shape reviewed across more sead builds'},
]
