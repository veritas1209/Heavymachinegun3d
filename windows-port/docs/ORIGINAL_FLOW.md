# Original Android flow reference

These captures were collected from the verified Android 4.4.2 Google APIs ARM
AVD on 2026-09-01. The APK and installed application data were not modified;
only normal tap input and `screencap` were used.

## Confirmed sequence

1. Animated title scene with the HEAVY GUNNER logo and TAP SCREEN prompt.
2. Main menu with CAMPAIGN, CHALLENGE, EXTRA, SETTINGS, and TERMS.
3. CAMPAIGN opens STAGE SELECT with NORMAL/HARD/EXTREME tabs and a 5-by-7
   stage grid.
4. Selecting stage 1-1 opens the MAGINOT LINE story screen. The recovered
   English `UI_cutScene01` texture contains the complete 480-by-320 frame,
   including the ruins background, briefing text, and TAP TO CONTINUE prompt.
5. Continuing opens ARMORY. START first presents a confirmation popup for the
   current loadout.
6. Confirming starts the mission with dual LMGs, a central radar, shield/hull
   bars, independent ammunition displays, target counter, enemy health bars,
   a fixed reticle, and sensor-driven camera aiming.
7. Android Back during the mission opens CONTINUE, CALIBRATE, and ABORT
   MISSION choices without leaving the rendered battlefield.
8. Unanswered enemy attacks eventually exhaust shield and hull, replacing play
   with MISSION FAILED and BACK/RETRY actions.
9. CHALLENGE opens SURVIVAL plus locked INFINITE A-E entries; EXTRA exposes
   RANKING, ACHIEVEMENT, INSTRUCTIONS, and WRITE A REVIEW; SETTINGS exposes the
   observed control, sound, quality, Y-axis, and sensitivity controls. TERMS
   does not leave the main menu in the tested build.

Reference captures:

- `analysis/reference/heavy-gunner-original-title.png`
- `analysis/reference/heavy-gunner-original-menu.png`
- `analysis/reference/heavy-gunner-original-campaign.png`
- `analysis/reference/heavy-gunner-original-stage11.png`
- `analysis/reference/heavy-gunner-original-armory.png`
- `analysis/reference/heavy-gunner-original-armory-confirm.png`
- `analysis/reference/heavy-gunner-original-playing.png`
- `analysis/reference/heavy-gunner-original-paused.png`
- `analysis/reference/heavy-gunner-original-failed.png`
- `analysis/reference/heavy-gunner-original-challenge.png`
- `analysis/reference/heavy-gunner-original-extra.png`
- `analysis/reference/heavy-gunner-original-settings.png`

An additional ten-frame time series established that the visible enemy group
moves across the upper field, individual enemies periodically dive close to
the camera, a successful attack produces a full-screen red flash and DANGER
indicator, and pause preserves the exact current battlefield.

The Windows OpenGL flow mirrors these seven observed states. The stage panel,
all 35 labels, difficulty labels, selection, and BACK control are reconstructed
from `UI_title_00_eng.png` plus a small code-native stroke font. ARMORY
and confirmation now use the recovered `UI_weapon_01/02/03_eng` and
`UI_popup_eng` atlases rather than static reference frames. Pause is
dynamic and freezes the live battlefield while compositing recovered popup
chrome. The failed state uses the recovered `BG_gameover_eng` frame.
