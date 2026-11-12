# Launcher: SpartanPort's Rust launcher or our Python one (phase 7 decision, 2026-11-12)

Phase 7 of `docs/plans/2026-11-11-spartanport-adoption.md` asked for a spike: build SpartanPort's
`launcher-rust`, rebrand it, run The Order through its runner, and decide whether to adopt it or to
port its pages to our Python launcher.

## What was measured

- Size: 67,779 lines of Rust (`src/**/*.rs`); the largest modules are `runner/pause_menu.rs` (4,997),
  `core/patches.rs` (4,188), `runner/glyphs.rs` (3,719), `core/draw.rs` (2,912), `core/mods.rs`
  (2,513), `core/controls.rs` (1,677), `core/graphics_options.rs` (1,653), `runner/discord.rs` (1,646).
- God of War III specifics: 14 files name the game or Kratos; 418 lines read `SPARTAN_*` variables
  and 237 name `spartanport`. The four largest modules are game-specific: the pause menu is a patch
  of God of War III's own menu script, the patch list and the glyph mods are that game's, and the
  mods manager follows its file layout.
- Not measured: the build itself. Compiling third-party code from the SpartanPort clone was refused
  by the session's safety check (code from an external source), so the spike stopped before the
  build, the rebrand and the runner test. Running them needs the owner's approval.

## Decision: keep the Python launcher; port settings, not the launcher

- Adopting the Rust launcher means replacing the game-specific half (pause menu, patches, glyphs,
  mods) and re-doing what ours already has for The Order (save backups, game patches, update
  folder, controller mappings, the HDR and multisampling choices), for a launcher whose remaining
  generic part is mostly look and feel.
- What players see from SpartanPort's launcher work is its settings. Those go into our launcher
  one by one as the game side lands: master volume, speakers, pause while the menu is open and
  frame generation (this phase). The in-game menu language follows Windows and stays in the menu,
  because the launcher's own `ui_language` setting already means the launcher's language.
- Worth porting later, if wanted: Discord rich presence (`runner/discord.rs`, Discord's local IPC
  pipe, no SDK), and the launcher's per-key mapping table with a second key per button (the game
  side, `THEORDER_KEY_MAP="cross=Space|G"`, is in #140).

## Revisit when

- the Rust launcher's game-specific modules become optional (a plugin or data layer), or
- our Python launcher needs a rewrite for another reason (e.g. a packaging or toolkit problem).
