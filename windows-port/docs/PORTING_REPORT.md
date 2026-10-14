# Heavy Gunner 3D Windows porting report

## Scope and integrity baseline

The Windows work is isolated under `windows-port`. The emulator and original APK are not modified.

| Item | Value |
|---|---|
| APK | `..\input\heavy-gunner-3d.apk` |
| APK bytes | 34,788,747 |
| APK SHA-256 | `23CF7EFF18C24F09947126D863FA5E27805787D32255D68F2909454461728873` |
| Android package | `com.com2us.HG` |
| Version | 1.0.8 (code 9) |
| Android target | API 7 |
| Native ABI | ARM32 `armeabi`, EABI5, little-endian, Thumb code |

## What was recovered

- 1,128 APK files totaling 37,260,405 uncompressed bytes.
- 592 asset files, including the full embedded `common/game_res` set.
- 667 DEX classes decompiled with JADX 1.5.6.
- Four ARM shared objects extracted intact.
- Dynamic symbols, imports, strings, ELF metadata, and selected function disassemblies recorded under `analysis`.
- 2,893 defined dynamic symbols remain in `libgame.so` despite the ELF being marked stripped.
- The native format probe recognizes all 180 duplicated/custom family assets (104 PMOD meshes, 2 PCAM cameras, 22 PEFT effects, and 52 PLIV actions) plus 8 A3D tables. All 22 PEFT files decode through their final frame-mask byte with exact end-of-file validation.

The rich dynamic symbol table is the most useful result. It exposes names such as `STATE_MAINROOM_Initialize`, `STATE_PLAY_Process`, `HG_GUN_RESOURCE_Load`, `HG_ENEMY_Process`, renderer functions, camera functions, and the exported JNI bridge.

This is not the original C/C++ source. Function names, sizes, machine code, data, and Java source-like output are recoverable; comments, original local names, build files, and exact high-level control structures are not.

## Runtime architecture found in the APK

The Java layer is primarily an Android platform adapter:

1. `com.com2us.game.MainActivity` loads `libgame.so`.
2. `WrapperRenderer.onSurfaceCreated` invokes `nativePreInit` and `nativeInit`.
3. Touch/key events are queued and sent through `nativeEvent`.
4. Timer callbacks enter through `nativeRender`.
5. Every rendered frame ends in `nativeReDraw`.
6. Java manages Android audio, sensors, text input, networking, activity lifecycle, and store/community modules.

The game logic and original OpenGL ES 1.x renderer are therefore in `libgame.so`, not in the Java classes.

### Native libraries

| Library | Bytes | Role |
|---|---:|---|
| `libgame.so` | 801,672 | Game state, gameplay, fixed-function GLES renderer, resource loaders, JNI bridge, bundled PNG/zlib support |
| `libnativeinterface.so` | 11,320 | Small native interface/loader bridge |
| `libARMPlugin.so` | 11,832 | Legacy SK Telecom ARM/licensing component |
| `libc2scommon.so` | 1,832 | Small Com2us common component |

`libgame.so` directly depends on `libGLESv1_CM`, `libdl`, `liblog`, the old Android `libstdc++`, `libm`, and `libc`. Windows cannot load this ARM ELF directly.

## State and input map

Recovered top-level state symbols include:

- Main room: main, campaign, challenge, achievement, scoreboard, settings.
- Play: gun select, gun upgrade, briefing, active play, pause.
- Lifecycle functions: initialize, process, draw, key press, and finalize.

The Android input bridge uses these event values:

| Event | Value |
|---|---:|
| Single touch | -2147483647 |
| Single drag | -2147483646 |
| Single release | -2147483645 |
| Multi-touch | -2147483644 |
| Multi-drag | -2147483643 |
| Multi-release | -2147483642 |
| Key down | 2 |
| Key up | 3 |

This mapping can be preserved in the Windows platform layer so reconstructed state code receives the same logical input events.

## Asset formats

Many final `.jpg` suffixes are camouflage rather than real JPEG data.

| Content | Recovered form | Current handling |
|---|---|---|
| Textures/UI | 135 ordinary PNG files | Directly reusable; WIC loader works |
| BGM | 15 Ogg Vorbis files | Original title and mission-1 tracks converted losslessly to PCM for MCI playback |
| Effects/voices | 39 WAV files | Directly reusable; WinMM validation works |
| Text tables | Plain text | Directly reusable |
| `.pmod` | `PMM` custom binary | Version 23 payload decoder complete; 104/104 pass |
| `.pcam` | `PCA` custom binary | Version 23 title-camera decoder complete; 2/2 pass |
| `.pliv` | `PLM` custom binary | 52/52 motion headers; UV/bone tracks plus 18/18 full vertex-animation copies decoded; zako01/02/03 bone actions and zako01/bMissile vertex actions render at native timing |
| `.peft` | `PEM` custom binary | Version 23 container fully consumed for 22/22 files; original timing drives six runtime effects while packed particle semantics remain under reconstruction |
| `.a3d` | Custom sheet/data binary | Native schema/record decoder complete and used by campaign runtime |

The first four custom formats share a recognizable family marker and version bytes. PMOD v23 uses quantized byte positions expanded through signed 16.16 bounds, signed-byte normals, 8.8 UVs, and 16-bit indices. PCAM v23 stores frame masks plus fixed or animated signed 16.16 scalar/vector tracks. Their loaders and renderers remain visible in the symbol map (`__boundPMOD`, `__renderPMOD`, `__renderPEFT`, `pactPlivVersion`, and A3D sheet functions), which provides anchors for the remaining formats.

## Current native render milestone

- The OpenGL compatibility executable preserves the original 480x320 logical coordinate system and reproduces the AVD's observed 800x480 (5:3) presentation aspect.
- The original title sky, ground, and player PMODs load as one shared-coordinate scene: 5,170 vertices and 3,956 triangles.
- Each scene part uses its recovered original PNG texture.
- `BG_00title_CAM.pcam` decodes to 460 frames at 30 fps with a 45-degree field of view and is replayed in the desktop renderer.
- Hidden render diagnostics on the Intel OpenGL 4.6 driver report `meshFrameGlError=0`; a BMP capture confirms the player and ground are visible from the recovered camera.
- `gun_fire.pmod` runs with its decoded 11-frame, 30 fps PLIV UV track and original 128x128 texture. All frames are observed in a complete loop with `gunFireLoopGlError=0`.
- The original zako01 and guided-missile PLIV vertex draw ranges are decoded and appended after their static PMOD ranges. Zako01 replays 48 vertices/24 triangles over ten 30-fps frames; bMissile replays 61 vertices/60 triangles over ten 30-fps frames. Hidden PLAYING validation reports `meshFrameGlError=0`.
- The original version-23 17-byte bone descriptor was separated from the later 18-byte layout by following `__pactLoadAniBone`'s version branches. Its self/parent/sibling/child links and consecutive vertex ranges cover the associated PMOD exactly: zako01 `80+185=265`, zako02 `83+248=331`, and zako03 `24+122+183=329`. Their 10/10/73-frame actions now transform the PMOD body through the recovered hierarchy at 30 fps; adjacent prepared vertex frames are interpolated just before drawing.
- PLIV header decoding passes 52/52 language copies. Ten standalone unflowed-UV sections fully decode, all 18 vertex-animation copies expand with checked ranges, and all three zako skeletal actions decode and render. Remaining multi-motion player skeletal variants are not complete.
- The runtime follows the AVD-confirmed TAP SCREEN → MAIN MENU → STAGE SELECT → STORY → ARMORY → LOADOUT CONFIRM → PLAYING → PAUSED/COMPLETED/FAILED sequence. Title/menu/stage/story are reconstructed from recovered assets. ARMORY and confirmation now compose the recovered English weapon and popup atlases dynamically, including selectable equipment slots. PAUSED freezes the live 3D battlefield and composites recovered popup chrome with the three observed actions.
- The original CHALLENGE, EXTRA, and SETTINGS submenu layouts were captured from the AVD and reconstructed as native states. SURVIVAL is playable from CHALLENGE as the decoded A3D record-40 80-second mission; legacy online EXTRA destinations remain non-networking placeholders. SETTINGS sound, Y-axis, and sensitivity now affect runtime audio/camera behavior. TERMS matches the original no-transition behavior.
- PLAYING exposes all 25 campaign missions from OVERTURE through LAST CALL. The native A3D loader reads the original title, objective, duration, target count, reward, enemy ID, pattern ID, and active group count from the stage/enemy/pattern tables on startup.
- Combat renders an 18-slot formation with A3D-selected zako01/02/03, middle01/02, guided missile, and boss01 PMODs plus both instances of the selected recovered weapon. All eight ARMORY weapons have recovered PMODs, model scaling, ammunition/cadence/damage behavior, original WAV firing audio where present, and their original `UI_target` reticles. Texture-size-aware UV scaling fixes the original 128/256/512-pixel texture coordinate convention.
- PLAYING now uses a PC-native single free reticle. LMB and RMB independently drive the left and right gun; both can be held simultaneously. Mouse movement aims directly, while entering an edge band pans the camera in place of gyroscope rotation. Each side has its own cadence gate, magazine, automatic reload timer, and HUD state. Sequential resolution provides same-frame overkill protection: if the first channel destroys its target, the second reacquires the nearest live target under the reticle. The decoded 11-frame `gun_fire` PLIV effect and direct/splash/pulse/shield/chain behavior remain connected.
- The previous synthetic eight-family sine movement has been removed. Campaign pattern 1 now executes the original type-1 radial entry and state-3 dive equations recovered from `HG_ENEMY_Add`/`HG_ENEMY_Process`, using the captured group angle/origin, Q16.16 child radius/height offsets, integer angle offsets, 25.931-second randomized group duration, and each child's recovered transition/dive timing. Pattern 5 (Hunted/Pendulum, movement type 2) now replays the original 16-slot Q16.16 coordinate trace captured directly from `TAG_HG_ENEMYINFO+0x18/+0x1c/+0x20` at group times read from `TAG_HG_ENEMYGROUP+0x5c`; this restores the independently moving formation and derives model tilt from the observed trajectory instead of inventing a replacement curve. The exact continuous type-2 accumulator equation after the captured 21.921-second window, boss type-2 variants, and native movement types 3-6 remain explicitly pending. Boss missions retain the recovered boss craft and laser visual while their exact native movement is pending.
- The stage grid labels all 35 buttons (`1-1` through `5-5`, `EX1` through `EX10`) and supports keyboard/mouse selection. All 25 campaign entries are enabled and completed entries receive an in-grid marker.
- Title BGM loops independently from effects. Campaign missions use their stage's recovered original BGM; stages 1 and 3 perform their head-to-body transition and loop the body segment. Completing target, survival, or boss objectives plays the original MissionComplete WAV and draws the recovered CONGRATULATIONS result sprite.
- Original hit, enemy-destruction, missile explosion, EMP, shield, thunder, player-hit, and boss-laser PNG payloads are loaded and animated/composited from native combat events.
- Recovered PEFT descriptors now supply original timing for hit (6 frames/30 fps), enemy destruction (22/30), missile explosion (26/30), shield (11/30), thunder (9/30), and boss laser (17/10). Runtime diagnostics require every descriptor to consume its complete source file before play begins.
- First-clear A3D reward points, six levels for each of eight weapons, the equipped weapon, 25 clear flags, and sound/camera settings persist in a local INI save. Upgrades affect damage, ammunition, cadence, and shield recovery; SETTINGS CLEAR DATA resets only this local progress.
- Exhausting shield and then hull enters the recovered 480x320 MISSION FAILED background with functional BACK and RETRY paths.
- Hidden captures for all 14 runtime states and overlays, all five stage themes, and all eight firing paths report `meshFrameGlError=0`. A separate startup sweep loaded all 25 theme/level combinations with `meshFrameGlError=0`; boss center-fire logs `hitEnemy=0`.

## Chosen Windows architecture

- Platform: native Win32 x64.
- Build: CMake + Ninja + MinGW-w64, all already installed locally.
- First validation renderer: Direct2D/WIC for lossless recovered PNG display.
- Target game renderer: a desktop OpenGL fixed-function compatibility layer matching the original OpenGL ES 1.x calls.
- Input: Win32 mouse/keyboard normalized to the recovered Android event numbers and the original 480x320 logical viewport.
- Sound effects: original WAV through WinMM. Original Ogg BGM is predecoded to 44.1 kHz PCM and streamed independently through MCI waveaudio with explicit head/body loop handling.
- Gameplay: function-by-function clean reconstruction following the recovered state and symbol map.

The Direct2D executable is deliberately an inspection scaffold. It proves native window creation, resource deployment, decoding, rendering, input, and sound without suggesting the original ARM gameplay code is already running.

## Milestones

1. **Completed:** immutable APK baseline, full extraction, Java decompilation, ELF/symbol inventory.
2. **Completed:** portable x64 executable with recovered PNG rendering, keyboard input, WAV playback path, and no non-system runtime DLL dependency.
3. **In progress:** custom formats. PMOD, title PCAM, the reusable A3D table loader, and the mechanically bounded PEFT v23 container decoder are complete. PLIV motion tables, unflowed UV tracks, all bone bind tables, zako01/02/03 hierarchy deformation, and all nine original vertex-animation sections are implemented. Remaining multi-motion player bone variants plus semantic expansion and native rendering of PEFT's packed particle records remain.
4. **In progress:** OpenGL compatibility renderer and reconstructed title/main-room state. The three-part title scene, original camera, atlas-composed TAP SCREEN/menu/submenus/numbered stage screens, dynamic ARMORY/confirmation, firing, pause, completion, and failure paths render; exact remaining compass/minimap records, lighting/effects, and network-backed submenu actions remain.
5. **Playable milestone:** gun/enemy/play reconstruction covers 25 campaign missions, target/survival/boss objectives, Challenge SURVIVAL, seven recovered enemy model families, all eight weapons, stage BGM, exact native type-1 formation/dive motion for campaign pattern 1, original runtime-traced type-2 formation motion for pattern 5, original zako01/02/03 skeletal actions, zako01/bMissile vertex actions, wave respawn, pause/failure/results, first-clear rewards, upgrades, persistent local saves, and the dual-trigger PC control/reload policy. Continuous type-2 equation completion, movement types 3-6, fine balance tuning, player skeletal variants, remaining per-enemy attack geometry, and retired network-backed features remain.

The emulator remains the behavioral oracle for checking animation timing, input semantics, camera positions, menu transitions, audio cues, and gameplay rules.
