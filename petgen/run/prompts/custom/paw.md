Create one horizontal animation strip for the pet `mecha-golden`, state `paw`.

Use the attached canonical base for identity. Use the attached layout guide only for slot count, spacing, centering, and padding; do not draw the guide.

Output exactly 4 full-body frames in one left-to-right row on flat pure magenta #FF00FF. Treat the row as 4 invisible equal-width slots: one centered complete pose per slot, evenly spaced, with no overlap, clipping, empty slots, labels, or borders.

Identity: same pet in every frame, exactly matching the attached canonical base: a tiny chunky low-resolution pixel-art golden retriever puppy based on a real dog, very light golden-cream fluffy coat, paler cream-white chest ruff and muzzle, black nose, dark gentle eyes, soft floppy ears, fluffy tail, big head and small body, a very short antenna with a small cyan tip on top of the head, and a tiny cyan heart on the chest. Preserve silhouette, face, proportions, markings, palette, material, style, and props.
Style: Pet-safe sprite: compact full-body mascot, readable in a 192x208 cell, clear silhouette, simple face, stable palette/materials, and crisp edges for chroma-key extraction. Style `pixel`: Pixel-art-adjacent digital mascot with a chunky silhouette, simple dark outline, limited palette, flat cel shading, and visible stepped edges. User style notes: big visible pixels (the whole pet is only about 48 pixels tall before upscaling), limited palette, flat colors with one lighter highlight shade, no black outline or a very soft darker edge, no anti-aliasing, minimal face, cozy and cute like the reference: a soft blob creature on a dark screen.
Animation continuity: keep apparent pet scale and baseline stable within the row unless the state itself intentionally changes vertical position, such as `jumping`. Move the pose within the slot instead of redrawing the pet larger or smaller frame to frame.

State action: Giving a paw (shake hands / 'otete'): sitting and lifting the right front paw forward toward the viewer, as if placing it in a person's open hand.

State requirements:
- Frame order: sitting looking up at the viewer; right front paw lifting; paw held forward toward the viewer (paw pad visible, proud happy face); paw coming back down.
- Body stays sitting and planted; only the paw, head tilt and expression change.
- No human hand, no props, no symbols, no detached effects.

Clean extraction: crisp opaque edges, safe padding, no scenery, text, guide marks, checkerboard, shadows, glows, motion blur, speed lines, dust, detached effects, stray pixels, or chroma-key colors inside the pet.
