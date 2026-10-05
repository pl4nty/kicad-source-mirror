# Fritzing importer test data

- `LF-HF-RFID-Detector.fzz`: RFID field detector board originally by Corey Harding, as published at
  https://github.com/pl4nty/RFID-Field-Detector. Licensed CC BY-SA 3.0.
- `fritzing-parts/`: the two stock part definitions and footprint images the sketch uses,
  copied from https://github.com/fritzing/fritzing-parts (commit 27535f2f). Licensed
  CC BY-SA 3.0.
- `synthetic.fzz`: a hand-written sketch with two bundled custom parts, a rotated through-hole
  part, a bottom-side SMD part, a rectangular board and a schematic ground symbol.

Expected positions for the RFID board come from the Gerber, drill and pick-and-place files that
Fritzing exported for this sketch, published in the same repository.
