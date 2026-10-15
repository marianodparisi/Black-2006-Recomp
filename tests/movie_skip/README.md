# Movie skip regression

These focused Windows tests compile the patched production movie input and
overlay sources, plus the authored BLACK wrapper in `src/recomp_manual.c`.
They need no game files, generated guest code, display, controller or saved game.

```powershell
cmake -S tests/movie_skip -B build-movie-skip -A x64
foreach ($configuration in 'Release', 'Debug') {
    cmake --build build-movie-skip --config $configuration
    ctest --test-dir build-movie-skip -C $configuration --output-on-failure
}
```

The default runtime path is `third_party/xboxrecomp`. Override it with
`-DXBOXRECOMP_DIR=<absolute pinned, patched checkout>` for another checkout.
The `Movie skip regression` workflow checks out the pinned runtime, verifies and
applies the repository patch, builds the production `xbox_video` and `xbox_input`
libraries, then runs all seven CTest regressions in Release and Debug.

- `movie-input` exercises Escape, Enter, Space, controller A/B/Start and left
  mouse: one-press startup skip, released second-press cutscene confirmation,
  repeat/hold rejection, timeout, session replacement, stale requests, end of
  movie, filter priority, report neutralization and focus loss/return. Physical
  and scripted mouse releases held across a transition must not leak menu taps.
  A synthetic resolver explicitly disables gameplay action mapping; no game
  control-map table is copied into the fixture.
  Additional source-backed cases enable `RECOMP_INPUT_SCRIPT_BACKGROUND=1` with
  a fully valid nonempty script. They verify held movement/fire and scripted
  mouse look survive window focus/capture changes, physical keyboard/mouse
  events never reach filters, pointer capture stays disabled, and virtual movie
  confirmations still reach the production movie filter. Disabled, malformed,
  absent and partial-script settings retain ordinary focus pause behavior; reset
  restores physical input. The gameplay cases use a synthetic resolver.
- `movie-overlay` supplies a one-texel synthetic font and tiny in-memory page to
  the production `pc_menu.c`. It checks safe margins, brightness, premultiplied
  alpha, letterboxing, ultrawide placement, resize, unchanged-pixel caching,
  invalidation, composition and exact restoration of cached page pixels.
  Synthetic SRT fixtures exercise the real parser across all nine stems and six
  locales, retain a 255-unit cue, and check explicit newlines, wrapped words,
  long-word splitting, safe picture bounds and same-client viewport resizing.
  Long captions are compared against a hint-only reference to verify that the
  lower-right prompt remains intact, hides independently, and returns alongside
  the caption. Cue expiry restores the exact underlying page pixels. Unrelated
  video settings and input services stay inactive through small harness stubs,
  without loading retail assets or writing settings files.
- `movie-wrapper` uses synthetic guest memory and a pump stub to check logo and
  cutscene classification, subtitle session/stem forwarding, ownership, EOF,
  reopen, decoder replacement, looping
  exclusion, the lab skip variable, stale requests, bounded names, guest registers,
  stack return and floating-point state. CMake derives the wrapper and font
  helper directly from the current production source into the build directory;
  no extracted `.inc` copies are published. Assertions remain enabled in Release,
  and the harness rejects a build with `NDEBUG` still defined.
- `movie-prompt-localization` checks the launcher prompt fragments for all six
  supported locales and the English fallback.
- `movie-picture` checks screen-height fitting of the opening cinematic at
  native and supersampled raster sizes, including integer/stretch settings,
  narrower windows, and unchanged sizing of other movie and gameplay frames.
  A custom 640x352 CPU raster stays unmodified; its dimensions cannot collide
  with the reduced 20:11 cinematic aperture marker.
- `movie-subtitle-catalog` parses every checked-in SRT, verifies the complete
  nine-stem by six-locale track matrix, and enforces the 255-unit parser limit
  while keeping a cue longer than the old 160-unit buffer in the catalog.

These regressions verify input/state/rendering contracts. They do not substitute
for playback and skip-transition checks using the user's own retail installation.
