# Heavy Gunner 3D Windows reconstruction

This directory is isolated from the working Android emulator. The original APK remains unchanged at `..\input\heavy-gunner-3d.apk`.

## Current milestone

- APK contents extracted and inventoried.
- ARM ELF headers, imports, strings, and demangled dynamic symbols captured under `analysis\native`.
- Original PNG assets recovered without modifying their image data.
- Native Win32/Direct2D/WIC asset inspector implemented.
- Desktop OpenGL renderer now loads the original title sky, ground, and player PMOD meshes.
- The original 460-frame, 30 fps title PCAM camera track is decoded and replayed.
- PLIV v23 motion headers decode for all 52 copies; all 18 vertex-animation copies expand with checked ranges. The original `gun_fire` UV animation, zako01/bMissile vertex actions, and zako01/02/03 skeletal PMOD deformation now run in OpenGL.
- The AVD-confirmed TAP SCREEN → MAIN MENU (including CHALLENGE, EXTRA, and SETTINGS) → STAGE SELECT → STORY → ARMORY → LOADOUT CONFIRM → PLAYING → PAUSED/COMPLETED/FAILED flow now runs natively.
- PLAYING exposes all 25 campaign missions (`1-1` through `5-5`) and reads their original titles, goals, timers, rewards, enemy IDs, active counts, and pattern IDs at runtime from `hg-stage.a3d`, `hg-enemy.a3d`, and `hg-enemy-pattern.a3d`.
- Combat renders original zako, middle, missile, and boss PMODs; the recovered zako01/02/03 skeletal actions; the ten-frame, 30-fps zako01 and bMissile PLIV vertex draw ranges; eight recovered player weapons; original audio/reticles; timed survival, target, and boss objectives; and the cockpit/pause/result flow. Campaign pattern 1 now follows coordinates and per-child dive timing recovered directly from the original ARM runtime; the remaining native movement types are tracked separately rather than represented by synthetic sine paths.
- First-clear rewards, six weapon levels, equipped weapon, cleared missions, and user settings persist in `build\bin\heavy-gunner-save.ini`. Upgrades change damage, ammunition, cadence, or shield recovery; click the ARMORY upgrade bar or press `U`.
- Recovered hit, enemy-destruction, missile, EMP, shield, thunder, player-damage, and boss-laser textures are composited during combat.
- All 22 PEFT v23 language copies now decode to their exact end; the Windows runtime reads the recovered hit, destruction, missile, shield, thunder, and boss-laser FPS/frame counts instead of assigning one shared hardcoded lifetime.
- The PC control layer uses one free mouse reticle: LMB fires the left gun, RMB fires the right gun, and both may be held together. Each side has independent cadence, ammunition, and automatic reload timing. When the first gun destroys a target, the second gun reacquires the nearest live target under the reticle instead of wasting the same-frame shot.

The executable is a playable clean-room reconstruction, not the original source code. Exact original movement types 2-6, PEFT particle parameters, player-weapon skeletal animations, lighting, balance, and retired online services are still being reconstructed or unavailable. Use Left/Right, Up/Down, Page Up/Page Down, or Enter to browse recovered textures. Space plays the recovered `Engine_Start` WAV. Escape closes it outside the game flow.

## Build and run

```powershell
Set-Location 'C:\Users\hajin\IT_Projects\android emul\windows-port'
.\build.ps1
.\run-opengl-port.cmd
```

The build uses the locally installed CMake/Ninja and MinGW-w64 C++ toolchain and does not install system-wide dependencies. Visual Studio's compiler is present, but its Windows SDK resource tools are not installed, so the self-contained MinGW-w64 toolchain is selected explicitly.

## Reconstruction facts

- `libgame.so` is a stripped ARM32 EABI5 shared object and cannot be loaded directly into a native Windows x64 process.
- Its dynamic symbol table is unusually rich: gameplay, state-machine, renderer, audio, camera, and JNI function names remain available.
- This makes function-by-function clean reimplementation practical, but it is still reconstruction from a binary, not recovery of the original C/C++ source.
- Most files ending in `.png.jpg` are ordinary PNG data. `.pmod`, `.peft`, `.pliv`, `.pcam`, and `.a3d` are engine-specific binary formats and need dedicated decoders.

See `analysis\native\libgame-symbols-demangled.txt` for the recovered function map.

The detailed findings and staged implementation plan are in `docs\PORTING_REPORT.md`.

`build\bin\pact_format_probe.exe` scans an asset directory and validates the recovered A3D/PMOD/PCAM/PEFT/PLIV families. PEFT v23 now decodes all 22 language copies through the motion table, packed vertex data, auxiliary/base records, repeated capacity and position-bounds block, and primary/optional secondary frame masks. Every PEFT reaches its exact file end.

The PMOD version 23 decoder reconstructs positions, normals, UVs, and triangle indices. All 104 PMOD copies in the APK pass payload and index-bounds validation. The PCAM version 23 decoder reconstructs fixed and animated scalar/vector tracks; both language copies of the title camera decode completely with no trailing bytes.

`run-opengl-port.cmd` starts the reconstructed game at its animated title. It keeps the original 480x320 logical coordinates while presenting the AVD-observed 5:3 (800x480) display aspect, uploads recovered textures through WIC, maps mouse input back to game coordinates, and shares the recovered WAV validation path. The Direct2D executable remains an asset inspector.

Press `M` in the OpenGL build to switch between the 2D texture inspector and the reconstructed title scene. The 3D mode draws the original sky, ground, and player meshes with their original textures and replays the 460-frame title camera loop.

Press `F` to run the recovered `gun_fire.pmod` with its original 11-frame, 30 fps PLIV UV animation and additive blend. The full 11-frame loop is covered by the automated runtime diagnostic. PLAYING deforms zako01/02/03 PMOD body vertices through the original PLIV bone hierarchy and appends the original zako01 and bMissile vertex-animation ranges. Remaining binary-format work is concentrated in PEFT's packed particle data and the multi-motion player-weapon PLIV variants; their unverified visual semantics remain separate from runtime rendering.

Press `G` to enter or leave the reconstructed game flow. Enter advances from
TAP SCREEN through MAIN MENU, STAGE SELECT, the selected mission briefing, ARMORY, loadout
confirmation, and PLAYING. Up/Down changes the menu selection; CAMPAIGN opens
the stage grid. During PLAYING,
Escape opens the recovered pause overlay; Escape or Enter resumes. Press
`G` again to return to the asset inspector. Start directly in this flow with
`build\bin\heavy_gunner_gl.exe --game`.

In PLAYING, the original weapon-specific reticle follows the mouse. Moving it
outside the central safe area pans the camera, so the center remains useful for
precision fire while the screen edges replace the phone's gyro rotation. SETTINGS
can switch edge panning off, toggle aim assist, change sensitivity, and reverse
the Y axis; SOUND controls BGM, weapon effects, and result voices. Hold LMB for
the left gun, RMB for the right gun, or both for simultaneous fire. Space fires
both guns once. The two channels have separate magazines, cadence gates, HUD
counters, and automatic reloads; `RLD` is shown while a side reloads. The eight ARMORY weapons use recovered PMODs,
ammunition/cadence/damage behavior, their recovered WAVs, and the original
11-frame `gun_fire` PLIV effect. All 25 campaign rows use the decoded A3D
objective, timer, target, reward, enemy, and pattern records. For campaign pattern
1, the Windows runtime uses the recovered type-1 radial entry equation, original
Q16.16 slot offsets, 25.931-second group duration, per-child state-3 transition
times, and dive durations. It does not map unrelated A3D fields to invented sine
paths. Boss rows use the recovered boss craft and laser behavior.
An unanswered attack uses the recovered player-hit effect and reduces shield
before hull. Use
`--reference` to compare the live mission against the captured original
gameplay reference frame, or `--capture-attack` to capture the 16.8-second
attack frame. Completing the current objective plays the original
MissionComplete voice, awards its first-clear points, saves progress, shows the
recovered CONGRATULATIONS panel, and Enter/click returns to STAGE SELECT.
If repeated dive attacks exhaust shield and hull, the recovered MISSION FAILED
screen replaces combat and offers BACK to stage select or RETRY with a reset
mission.
The title track loops independently. Campaign missions play their stage's
recovered original track; stages 1 and 3 use their head-to-looping-body split.
CHALLENGE → SURVIVAL is playable as the original A3D record's 80-second
stage-1-background survival mission, including ARMORY, pause, retry, and result
paths.

For an automated hidden render check, run `build\bin\heavy_gunner_gl.exe --mesh --capture`. It writes `opengl-runtime.txt` and `opengl-mesh-capture.bmp` beside the executable.

Use `build\bin\heavy_gunner_gl.exe --fire --capture` for the PLIV effect check; it writes `opengl-fire-capture.bmp`. The confirmed PLIV layout is documented in `docs\PLIV_FORMAT.md`.

Use `--game`, `--menu`, `--challenge`, `--extra`, `--settings`,
`--mission`, `--story`, `--armory`, `--confirm`, `--playing`,
`--paused`, `--complete`, `--failed`, or `--survival` with
`--capture` for individual state checks. (`--mission` is retained as an
automation alias for STAGE_SELECT.) The pause screen freezes and retains the
current 3D battlefield rather than substituting a static reference frame.
Use `--theme1` through `--theme5` plus `--level1` through `--level5` with
STORY/PLAYING, and `--weapon1` through `--weapon8` with PLAYING. All 25 campaign
combinations load successfully; all 14 current states/overlays, five stage themes,
and eight firing paths report `meshFrameGlError=0` on the Intel OpenGL
validation system. Add `--shoot` to a PLAYING check to fire both channels and
cover the recovered weapon/PLIV firing frame.

The control and throughput rationale, including the worst target-count mission,
is recorded in `docs\PC_CONTROLS_AND_BALANCE.md`.
