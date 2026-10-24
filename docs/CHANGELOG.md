# Changelog

Versions follow `VERSION` in the repository root; the game and the launcher show it, and the
update notice compares it with the newest GitHub release. Dates are when the work landed.

## Unreleased

- 60 fps on slower PCs: if your PC can't keep up, the game now drops to 30 fps for a bit instead
  of slowing down (that's what made the sound slow and crackly). it tries 60 again after a few
  seconds
- stick deadzone setting for each stick (Esc > Controls, or left_stick_deadzone /
  right_stick_deadzone in controls.cfg). the left stick also feels a lot smoother near the
  middle now, it used to snap

## 0.9.1 — 2026-10-24

two things people asked for after the first release:

**cheats menu**
press Esc and go to Cheats. you've got no damage, infinite ammo, infinite grenades, infinite
liberation and max BP. turn them on and off whenever you want, they stay how you left them.
the codes come from the cwcheat database so big thanks to the people who found them

**new camera option**
Esc > Camera style, or tick "Modern camera" in the launcher. the camera sits over Aya's
shoulder, stays where you point it instead of swinging back behind her, and she walks where
you're looking. when you aim, the mouse moves the aim too. the old camera is still the default
if you like it better

heads up: you need to press "Prepare game" one more time after updating (the launcher will
tell you). takes a couple of minutes, your saves and settings stay

## 0.9.0 — 2026-10-23

First numbered version: everything up to now, plus a round of PC features.

**Download and play**
- The release package holds no game code: the launcher's **Prepare game** builds the game from
  your own disc image on your PC (decrypts the program from the ISO, translates and compiles it
  with the bundled kit, about 3-10 minutes, nothing downloaded). Updates of the PC runtime don't
  need the game prepared again; the launcher says when they do.
- Unlock all extras (launcher): every costume, extra DNA boards and Cheat Codes; your progress
  is kept

**PC features**
- Launcher (`The 3rd Birthday.exe`): finds or asks for your disc image and checks it, settings,
  save list with backup and restore, texture packs and patches on/off, update status, Play.
  Its background is the game's PSP cover art (Aya), read from your own disc image, with blood
  effects drawn over it; the window icon is the disc's icon
- Update notice: the game and the launcher say when a newer release is on GitHub (once a day,
  can be turned off)
- Mouse camera: click the window, the mouse turns the camera; left button fires, right aims
- Smooth WASD turning; mouse sensitivity / invert; all keys and buttons remappable, mouse
  buttons too (left, right, middle, side buttons), in the game (Esc menu > Controls) or in the
  launcher (Change buttons...); saved in `controls.cfg`
- Widescreen / ultrawide (Esc menu > Aspect ratio): the game renders a wider view and its own
  culling widens with it; the HUD keeps its shape in the middle; screen effects (Overdive tint,
  Game Over, finale backdrop) cover the whole window
- 60 fps with interpolated limbs (skinned meshes blend per vertex)
- FPS counter and a glow (bloom) toggle in the Esc menu
- Safe saves: written to a temporary file and swapped in; every save is backed up to
  `memstick/BACKUP/<save>/<date-time>/` before it is overwritten (newest 10 kept)
- Mods: HD texture packs (`textures/<hash>.png`, texture dump), memory patches
  (`patches/*.txt`), compiled code mods that can replace game functions (`mods/README.md`)
- Data Install (Extra menu) works, and Use Install Data reads from it like on a PSP
- Import saves from a real PSP or PPSSPP (`tools/save_import.py`); the port's saves load in PPSSPP
- Double-click start (no scripts needed), portable package (`scripts/package.ps1`), one-step
  setup for a fresh clone (`scripts/setup.ps1`)

**Accuracy**
- System save/load screen (slot list, overwrite confirmation) like the PSP's
- Framebuffer alpha is the stencil buffer, as on the PSP; the game's stencil writes are emulated
- Exact doubled-alpha blending (dual-source): the finale scenes match the real game
- All 111 scenes load and render; 22 compared frame by frame with PPSSPP
- Crash fix in later episodes (thread stacks no longer in the volatile memory area)

**Performance**
- Heavy scenes hold 30 fps (GE front end, vertex readback, debug hooks, -O2 runtime)

## Before 0.9.0

September–October 2026: static recompilation of the game, the runtime (HLE, scheduler,
audio, movies, saves), the Vulkan renderer with higher internal resolution and 60 fps
interpolation, the twin-stick camera, controller and keyboard support, the settings menu.
`PLAN.md` has the dated log of every step.
