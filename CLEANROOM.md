# Clean-room rules

Ember is an original engine. It reads game data the user already owns; it ships none of it.

1. **No original code.** Nothing decompiled, disassembled or copied from the original executables
   may enter this repository, in any form (including "cleaned up" or renamed). Do not paste
   decompiler output into issues, comments or commit messages either.
2. **No original data.** No game assets, test fixtures cut from game files, screenshots of
   extracted textures, or hashes/dumps large enough to reproduce content.
3. **File formats are documented from independent sources** (public community documentation,
   or observation of the user's own files with ordinary tools such as a hex viewer), written up
   in `docs/formats/` in our own words with the source cited. Parsers are written from those
   notes, not from anybody's decompilation.
4. **Behaviour is specified, then implemented.** Game rules are written as observed behaviour
   ("the player rolls for N frames") before they become code.
5. **The runtime never redistributes data.** Game files are read from a folder on the device
   that the user filled themselves, and only held in memory.
6. **Third-party code** (renderers, math, formats libraries) must have a compatible license and
   is recorded in `THIRD_PARTY.md`.

These rules are a working discipline, not legal advice. Before publishing, have someone qualified
review the project, particularly around trademarks (do not use the original game's name or logo
in the project's name or branding).
