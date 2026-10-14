# Heavy Machine Gun 3D compatibility work

This repository archives the compatibility research and clean-room Windows reconstruction work performed for the legacy Android game **Heavy Gunner 3D**.

## What is included

- `android16-port/diagnostics/`: reproducible Android/ART compatibility patchers and diagnostic helpers.
- `android16-port/tools/patch-wrapper-renderer-smali.py`: applies the Java-side gyro bridge to a user-provided decompiled `WrapperRenderer.smali`.
- `windows-port/`: the native Windows reconstruction source, format decoders, analysis scripts, and technical notes.
- `releases/Heavy-Gunner-3D-Android-compat.apk`: the final APK that was verified in the VPhoneOS test environment.

## What is intentionally not included

The original APK, extracted assets, standalone native game libraries, full decompiled game code, Android SDK/AVD images, signing keys, and intermediate binaries are not redistributed. Supply a legally obtained copy of the game and the required build tools yourself. The final compatibility APK is included at the owner's request so the completed result remains directly accessible.

The Windows reconstruction also needs the original assets at runtime; this repository alone is not a redistributable game package.

## Verified Android compatibility result

The final build was tested through ADB on a VPhoneOS Android 7.1.2 ARM environment. Startup, menus, touch input, combat/enemy movement, and gyroscope aiming were confirmed working.

The final compatibility chain preserves the original game calculations while addressing legacy assumptions that fail on ART/newer environments:

1. legacy file-size handling is replaced with an end seek;
2. JNI `int[]` asset metadata is synchronized when ART returns a detached copy;
3. unsafe timer-list head removal is repaired;
4. the native clock path is made ART-compatible;
5. the three-axis sensor values are transported losslessly through tagged `nativeRender` packets and synchronized back into the original native float array.

The Java/smali bridge is applied separately with `patch-wrapper-renderer-smali.py`; the native half is implemented by `patch-native-jni-float-array-sync.py`.

## Rebuilding the Android compatibility APK

Prerequisites: PowerShell, Python 3, Java/keytool, Android build-tools containing `zipalign` and `apksigner`, smali 2.5.2, and a legally obtained original APK.

1. Decode the APK's `classes.dex` to a smali directory with smali/baksmali tooling.
2. Apply the gyro bridge:

   ```powershell
   python .\android16-port\tools\patch-wrapper-renderer-smali.py <path-to-WrapperRenderer.smali>
   ```

3. Adjust or pass the local paths in `android16-port/diagnostics/build-fd-instrumented-apk.ps1` and build with the final compatibility switches:

   ```powershell
   .\android16-port\diagnostics\build-fd-instrumented-apk.ps1 `
     -InputApk <original.apk> `
     -SmaliDirectory <decoded-smali-directory> `
     -OutputDirectory <output-directory> `
     -PatchNativeFileGetSize `
     -PatchNativeJniArraySync `
     -PatchNativeTimerHeadUaf `
     -PatchNativeCurrentTime
   ```

The script creates and uses a debug signing key. Do not use that key for production distribution.

## Reference hashes

These hashes identify the exact private inputs/outputs used during the successful test; the files themselves are not included.

| Artifact | SHA-256 |
|---|---|
| Original APK | `23cf7eff18c24f09947126d863fa5e27805787d32255d68f2909454461728873` |
| Final tested compatibility APK | `580ab1026d21d722948348313da884a160a24d76c63524818262a5a59def81af` |

## Windows reconstruction status

The Windows directory is an incomplete clean-room reconstruction, not recovered original source. It includes the recovered binary-format decoders and the current native gameplay prototype, but exact parity is not complete and the original proprietary assets are deliberately absent. See `windows-port/README.md` and `windows-port/docs/PORTING_REPORT.md` for details.

## Legal note

This project contains compatibility and interoperability work only. Heavy Gunner 3D and its original code, artwork, audio, and trademarks remain the property of their respective rights holders.
