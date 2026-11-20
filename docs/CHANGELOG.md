# Changelog

Versions follow `VERSION` in the repository root; the game and the launcher show it, and the
update notice compares it with the newest GitHub release. Dates are when the work landed.

## 0.9.16 - 2026-11-20

- 60 fps: the GAME OVER title doesn't get doubled anymore, and the same fix covers things
  showing up twice for a frame in busy fights. when something blinks in or out, the in-between
  frame could pair it with the next one over and draw it halfway. now it only moves things that
  are really the same. thanks ijezabella for the video!

no need to press "Prepare game" again this time

## 0.9.15 - 2026-11-19

- no more ghosting with the glow on at higher resolutions. the glow is a small blurred copy
  of the picture laid over it, and above 1x it kept too much detail, so it showed up as a sharp
  second copy a few pixels off (easy to see on lights). it's soft like the original again.
  thanks for the report in #21
- Load Game with no saved game now says "There is no saved data." instead of dropping you
  back at the logos with no message. after that it goes back to the logos like the original
  does. thanks for the report in #16
- if the game crashes, run.err now also says which dll it happened in (helps with overlays
  like reshade, #17)

no need to press "Prepare game" again this time

## 0.9.14 - 2026-11-19

- right stick camera reacts faster. it used to ease into the turn over about a tenth of a
  second, now it's twice as quick. thanks for the report in #20

no need to press "Prepare game" again this time

## 0.9.13 - 2026-11-19

- full analog left stick! the original game keeps aya running straight while the stick points
  within about 35 degrees of where she's going (ppsspp does the same), so small corrections did
  nothing and it felt like a d-pad. now she runs exactly where you point. Esc > Left stick:
  Original puts the old behaviour back. thanks for the report in #15

no need to press "Prepare game" again this time

## 0.9.12 - 2026-11-18

- 60 fps: scrolling the pause menu doesn't make the rows go dark anymore. the menu draws a lot
  of the same thing (46 rows of text) and the fix from 0.9.11 only looked at up to 32, so the
  rows still got paired with the wrong ones. thanks ijezabella!
- free aim: no more camera jolt when you start aiming. the game showed a few frames looking
  where aya was facing before, then jumped to where you aim. now she turns first and the camera
  goes into the aim smoothly. thanks for reporting it in #15

no need to press "Prepare game" again this time

## 0.9.11 - 2026-11-17

- xbox button prompts! Esc > Button prompts: Xbox. the game's PSP buttons in menus and text
  turn into A / B / X / Y, LT / RT, VIEW and MENU. the icons are drawn by the port, nothing new
  to download. thanks for the idea in #15
- 60 fps: the "NEW" tags in the mission menu don't jump around anymore. when one of them
  blinked the in-between frame paired it with the wrong one and drew it halfway between them,
  same for some menu rows going dark. thanks ijezabella for the videos!
- 60 fps on some AMD integrated graphics: the game sent the gpu more settings per draw than
  those cards allow, now it fits

no need to press "Prepare game" again this time

## 0.9.10 - 2026-11-16

- modern / centred camera: no more seeing through walls. the camera now gets pushed in by walls
  the same way the original camera does, and moves back towards the middle when it's tight.
  thanks ijezabella for pointing it out

you'll need to press "Prepare game" once more after updating

## 0.9.9 - 2026-11-16

- Overdive effects: the pink lines over the top of the screen and the noise on the right edge
  are gone. the effect was reading the wrong image (an old copy instead of what was just drawn).
  this was on every graphics card, not just AMD. thanks ijezabella for the run.err!
- texture upscaling like in ppsspp! Esc > Texture upscaling: 2x, 3x or 4x (xBRZ). every texture
  gets scaled once when it loads, so it doesn't slow the game down, maybe a tiny hitch the first
  time you see something new. off by default

no need to press "Prepare game" again, just update and play

## 0.9.8 - 2026-11-15

- AMD cards at 60 fps: barricades (and other things the game draws twice on top of each other)
  were flashing white. the in-between frames now put both layers at exactly the same depth

no need to press "Prepare game" again, just update and play

## 0.9.7 - 2026-11-15

- new camera style "Centred": the camera stays where you point it like the modern one, but Aya
  stays in the middle of the screen instead of over the shoulder. Esc > Camera style
- modern / centred camera: letting go of aim (or coming out of Overdive) now eases back instead
  of snapping
- texture filtering option, one for the 3D and one for the HUD / menus: as the game does it,
  sharp pixels, or smooth. Esc > Texture filtering
- if the game was prepared from the wrong disc image (not the US 1.00 release) it now tells you
  instead of just closing
- RivaTuner's overlay makes the game close right at the start. if it's running the game now
  says so in run.err, and the readme says how to turn it off for the game
- the Esc menu got too long again and the last lines were cut off, fixed

no need to press "Prepare game" again, just update and play

## 0.9.6 - 2026-11-03

- AMD cards: fixed (hopefully!) the black squares flickering over the screen. the game was
  telling the graphics card to draw stuff in an order NVIDIA didn't care about but AMD does.
  thanks TheDimensioner for the video

no need to press "Prepare game" again this time, just update and play

## 0.9.5 - 2026-11-01

- free aim! hold L (right mouse) and you aim where the camera is looking, then the mouse or the
  right stick moves the aim around, left/right and up/down, and the shots go where the crosshair
  is. the original game can't do this at all, you could only aim the way Aya was facing.
  if the game locks onto an enemy when you start aiming it keeps the lock until you move the
  mouse. on by default, turn it off in Esc > Free aim if you want the old aiming back
- the sniper scope and the launchers still aim like before (mouse moves them)
- fixed: in 0.9.1-0.9.4 moving the mouse while aiming made Aya walk sideways instead of aiming,
  sorry about that

you'll need to press "Prepare game" once more after updating

## 0.9.4 - 2026-11-01

- widescreen: characters don't tear apart anymore when you maximize the window or go fullscreen
  after the game started. small pieces like hair and straps were getting stretched separately
  from the rest of the model. thanks WenRaia for the run.err, that's what found it

no need to press "Prepare game" again this time, just update and play

## 0.9.3 - 2026-10-25

small one:

- the Esc menu shows up again. it got too tall for the screen after the last few options were
  added, so it just didn't draw (the game was paused with nothing on screen)
- the Max BP and Infinite Liberation cheats actually work now. they were writing to the wrong
  spot in memory (an unused one, so nothing broke, they just did nothing)

you'll need to press "Prepare game" once more after updating

## 0.9.2 - 2026-10-24

fixes for the stuff you reported, thanks for all the reports!

- 60 fps on slower PCs: if your PC can't keep up, the game now drops to 30 fps for a bit instead
  of slowing down (that's what made the sound slow and crackly). it tries 60 again after a few
  seconds
- stick deadzone setting for each stick (Esc > Controls, or left_stick_deadzone /
  right_stick_deadzone in controls.cfg). the left stick also feels a lot smoother near the
  middle now, it used to snap
- texture packs made for PPSSPP work now, just copy the pack into the textures folder (see
  mods/README.md)

you'll need to press "Prepare game" once more after updating, same as last time

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
