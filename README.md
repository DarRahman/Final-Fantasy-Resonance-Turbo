# Final Fantasy Resonance Turbo

[![Release](https://img.shields.io/badge/release-v1.1.0-blue?style=flat-square&logo=github)](https://github.com/DarRahman/Final-Fantasy-Resonance-Turbo/releases)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square&logo=windows)](#installation)
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)

A lightweight, standalone High-Speed Turbo mod for **FINAL FANTASY RESONANCE** on PC.

## What Does This Mod Do?

This mod adds a standalone **Turbo Mode** to *FINAL FANTASY RESONANCE* on PC, letting you speed up the entire game to **2.5x** (or any custom speed) with a single button press.

While many modern JRPGs now include a fast-forward feature, this mod's pacing and seamless integration are heavily inspired by the excellent High-Speed Mode seen in Nihon Falcom's *Trails in the Sky FC* (2025) and *Trails in the Sky SC* (2026) remakes.

Unlike standard in-game options that only increase battle speed, this mod accelerates the engine's internal tick rate so everything runs faster together in sync:
- Overworld and dungeon movement.
- Turn-based combat and enemy turns.
- Dialogue text and cutscenes.

You can toggle it on or off anytime with a single tap of **RT** on your controller or **F1** on keyboard.

---

## Preview

### 1. Turbo Mode Toggle (2.5x)
![Turbo Toggle Demo](assets/demo.webp)

*Full 1080p 60 FPS video showcase available in [`assets/Final_Showcase_Demo.mp4`](assets/Final_Showcase_Demo.mp4).*

### 2. In-Game Speed Cycling (New in v1.1.0)
![Speed Cycling Demo](assets/demo_speed_cycling.webp)

*Speed cycling gameplay showcase available in [`assets/speed_cycling_demo.mp4`](assets/speed_cycling_demo.mp4).*

<table>
  <tr>
    <th width="33%">Field Exploration</th>
    <th width="33%">Turn-Based Combat</th>
    <th width="34%">Speed Cycling HUD (3.5x)</th>
  </tr>
  <tr>
    <td><img src="assets/preview_field.webp" alt="Field Exploration" /></td>
    <td><img src="assets/preview_battle.webp" alt="Turn-Based Combat" /></td>
    <td><img src="assets/preview_speed_cycling.webp" alt="Speed Cycling HUD" /></td>
  </tr>
</table>

---

## How to Use

- **Xbox Controller**:
  - Tap **RT (Right Trigger)** to toggle Turbo on or off.
  - While Turbo is active, tap **L3 (Left Stick Click)** to cycle through speed presets (`1.5x` → `2.0x` → `2.5x` → `3.0x` → `3.5x` → `4.0x`).
- **Keyboard**:
  - Press **F1** to toggle Turbo on or off.
  - Press **F2** to cycle through speed presets.
- When active, a clean `◆ TURBO MODE` badge appears in the top bar, accompanied by a brief non-intrusive speed notification that automatically fades out after 1.5 seconds.
- Tap the toggle button again to return to normal 1.0x speed.

---

## Installation

1. Download `Final-Fantasy-Resonance-Turbo.zip` from [Releases](https://github.com/DarRahman/Final-Fantasy-Resonance-Turbo/releases).
2. In Steam, right-click **FINAL FANTASY RESONANCE** → **Manage** → **Browse local files**.
3. Open the binary folder (where `FFRS-Win64-Shipping.exe` is located):
   ```text
   FINAL FANTASY RESONANCE\FFRS\Binaries\Win64\
   ```
4. Extract the zip files directly into that folder:
   - `dwmapi.dll`
   - `turbo_config.ini`
   - `assets/` (contains HUD indicator and speed toast badges)
5. Launch the game through Steam.

*Note: This mod modifies 0 bytes of original game files on disk. To uninstall, simply delete `dwmapi.dll`, `turbo_config.ini`, and `assets/` from that folder.*

### Using alongside UE4SS?
If you have UE4SS installed (for mods like Fast Travel or Auto-Battle):
1. In `FFRS\Binaries\Win64\`, rename UE4SS's original `dwmapi.dll` to: `dwmapi_ue4ss.dll`
2. Extract this mod's files into that same folder.

FFRS Turbo will automatically detect `dwmapi_ue4ss.dll` on game launch and load both mods together.

### Steam Deck & Linux (Proton)
Add this to the game's **Launch Options** in Steam:
```bash
WINEDLLOVERRIDES="dwmapi=n,b" %command%
```

---

## Configuration (`turbo_config.ini`)

All settings can be customized without recompiling. Open `turbo_config.ini` in any text editor:

- **`SpeedMultiplier`** (Default: `2.5`): Base speed multiplier when Turbo is active. Official presets: `1.5`, `2.0`, `2.5`, `3.0`, `3.5`, `4.0`. Setting a custom unlisted value (e.g. `5.0`) locks the engine to that speed and disables cycling.
- **`GamepadButton`** (Default: `RIGHT_TRIGGER`): Controller toggle button. Set to `NONE` or `OFF` to disable gamepad input entirely for keyboard-only play. Supported: `RT`, `LT`, `R3`, `L3`, `LB`, `RB`, `DPAD_UP`, etc.
- **`KeyboardKey`** (Default: `0x70` = F1): Keyboard toggle key in Windows virtual-key hex code.
- **`EnableSpeedCycling`** (Default: `1`): Set to `0` to completely disable in-game speed switching and lock the mod to a single speed.
- **`CycleSpeedKey`** (Default: `0x71` = F2): Keyboard hotkey to cycle speeds.
- **`CycleSpeedGamepad`** (Default: `LEFT_THUMB` = L3): Controller button to cycle speeds.
- **`AlwaysShowSpeed`** (Default: `0`): Set to `1` to keep the speed badge visible continuously below the Turbo indicator while active.
- **`DebounceMs`** (Default: `250`): Minimum delay between button presses in milliseconds to prevent accidental double-toggling.

---

## Gameplay Notes & Limitations

- **3D Video Cutscenes**: Pre-rendered cinematics (such as special Vision attacks) stream audio on real-time hardware clocks. Because Turbo accelerates the game engine timeline, the video may finish playing before its audio track completes. If you want to watch a cinematic cutscene normally, tap **RT** to pause Turbo, or use the controller's **View / Back** button to skip the animation.
- **In-Game Battle Speed**: Keep the game's built-in battle speed set to **1x**. Turbo Mode will speed up the entire battle smoothly at 2.5x.
- **File Safety**: This mod does not touch or modify any original game files.

---

## Contributing

Contributions, bug reports, and suggestions are welcome! Feel free to open an issue or submit a pull request.

---

## Author

[@DarRahman](https://github.com/DarRahman)

---

## Note

This mod was developed with AI assistance and thoroughly tested, audited, and verified in-game on PC by [@DarRahman](https://github.com/DarRahman).

---

## Credits

- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu — The minimal x86/x64 API hooking library for Windows.

---

## License

Released under the [MIT License](LICENSE).
