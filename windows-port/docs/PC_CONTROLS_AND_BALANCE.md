# PC controls and balance policy

## Final control mapping

- Mouse movement positions one shared reticle.
- LMB fires the left gun and RMB fires the right gun. Either button can be held independently; holding both fires both channels.
- The central safe area is direct aim only. Moving the reticle into an edge band pans the camera, replacing gyro rotation without requiring WASD.
- The largest recovered reticle is clamped inside a 36-pixel logical margin so it remains visible during edge panning.
- Escape pauses. The pause menu's CALIBRATE action recenters both the camera and reticle. Space fires both channels once as an accessibility fallback.
- SETTINGS exposes EDGE PAN/FIXED camera behavior, aim assist, Y-axis direction, and sensitivity.

## Dual-fire rules

Each gun owns its cadence timestamp, magazine count, reload flag, and reload completion time. Empty magazines automatically refill after a weapon-specific delay; reload timers freeze while paused. This intentionally removes a finite reserve-ammunition failure condition from the touch-to-mouse conversion while retaining tactical interruptions and separate left/right HUD feedback.

Shots resolve left then right within the same timer frame. A dead enemy is removed from target selection immediately, so if the left shot kills it the right shot searches again and can hit another live enemy near the same reticle. If the first shot only wounds the target, the second continues focused fire. This is the overkill protection needed for a shared reticle without turning both guns into a single synthetic damage event.

## Throughput check

The heaviest decoded target mission requires 400 kills in 300 seconds, or 1.33 kills per second. The base Crusader LMG fires two independent 1-damage channels every 90 ms. Against a 3-HP standard enemy its perfect-hit ceiling is:

`2 / 0.090 / 3 = 7.41 kills per second`

At only 25% effective accuracy this is about 1.85 kills per second, leaving roughly 39% margin over the mission requirement before upgrades. Its two 500-round magazines last 45 seconds of continuous dual fire and both reload in 1.1 seconds. The reload duty cycle therefore retains about 97.6% of the theoretical rate. The 850 ms wave-slot respawn delay with 15 active slots caps supply near 17.6 enemies per second, well above required and weapon throughput.

This demonstrates that the shared-reticle conversion does not make the worst target-count stage mathematically impossible. Practical play still needs tuning because enemy motion, visibility, incoming damage, and special-weapon behavior affect realized accuracy. Runtime verification specifically covers independent channel counts, automatic reload/restart, and same-frame target reacquisition.

The Shield Recharger is intentionally non-offensive. The loadout confirmation blocks it for target and boss missions and explains that an offensive weapon is required; it remains selectable for survival play. This prevents a support-only loadout from creating a separate mathematically impossible campaign start.
