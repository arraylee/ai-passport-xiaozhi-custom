# FoloToy companion faces

Exactly two user-selected characters: the red-scarf woman and silver-haired
headphones man. The default is the woman. Names, voices, and roles remain cloud
settings; appearance does not create a second agent.

`source/girl-faces.png` and `source/boy-faces.png` are the original transparent
outputs from the built-in image_gen tool, generated using the user's earlier
character sheet as identity reference. Generation specifications are preserved
in `PROMPTS.md`. No API fallback or substitute artwork was used.

`tools/prepare_companion_faces.py` exports six aligned 144×144 RGBA frames per
character: idle, blink, listen, think, talk, happy. It detects each separated head,
crops transparent gutters, resizes once, and preserves generated alpha. The
resulting boxes and semantic order are recorded in `faces.json`. These committed
native PNGs are sufficient to build; regeneration is optional.

`tools/companion_pack.py` converts the twelve frames to RGB565A8. Their total is
746,496 bytes (729 KiB), mapped directly from read-only Flash. The device never
scales a face and does not allocate a GIF decoder or full-frame animation buffer.

CPR1 container version 2 has fourteen entries in its 128-byte header: two font
bitmaps, then six expressions for each of the two characters. Release validators
also accept the old version 1 static-portrait package; firmware accepts only the
resource format and fingerprint built alongside it.

The old `avatar-01.png` / `avatar-02.png` are the prior full-body source references,
excluded from the pack. The demo homepage avatar pack is unchanged.
