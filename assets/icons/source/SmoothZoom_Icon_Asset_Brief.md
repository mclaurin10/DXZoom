# SmoothZoom — Visual Asset Brief

## Design Direction

**Style:** Windows 11 Fluent Design — monoline, single-weight stroke (~1.5px at 16px canvas), no gradients, no fills competing with system tray icons. Clean, geometric, optically balanced.

**Motif:** Magnifying glass or concentric-circles zoom indicator. Should read clearly at 16×16 and scale gracefully to 256×256. Avoid tiny detail that disappears at small sizes.

**Color:** Monochrome white for tray icons (Windows renders on its own background). The application icon (Explorer, taskbar, installer) can use a single brand accent color — a blue in the #0078D4–#3A96DD range fits the Windows 11 system palette without clashing.

---

## Asset 1 — Application Icon (`.ico` resource)

**Where it appears:** File Explorer (the `.exe` file), Alt+Tab switcher, Task Manager process list, taskbar (if pinned), Start Menu shortcut, installer Add/Remove Programs entry.

**Format:** Single `.ico` file containing multiple resolution layers:

| Layer | Size | Notes |
|-------|------|-------|
| 1 | 16×16 | Explorer small icons, Task Manager |
| 2 | 20×20 | Start Menu small tile (125% DPI) |
| 3 | 24×24 | Explorer default view at 150% DPI |
| 4 | 32×32 | Explorer medium icons, desktop shortcut |
| 5 | 48×48 | Explorer large icons |
| 6 | 256×256 | Explorer extra-large icons, Alt+Tab, PNG-compressed inside ICO |

**Image generator prompt concept:**

> A minimal magnifying glass icon in the Windows 11 Fluent Design style. Single-weight monoline stroke, slightly rounded corners on the glass rim. The handle points to the lower-right at roughly 45°. Inside the glass lens, two concentric arcs or a subtle "zoom in" plus sign suggest magnification. The icon uses a single blue accent (#0078D4) for the lens rim and a lighter blue or white fill for the glass area. Clean vector geometry on a transparent background. No shadow, no gradient, no 3D perspective.

**Variant:** Consider a version where the lens interior contains a stylized "S" or "SZ" monogram if the plain magnifying glass feels too generic. Test both at 16×16 to see which reads better.

---

## Asset 2 — System Tray Icon, Idle State

**Where it appears:** Windows notification area (system tray), always visible while SmoothZoom is running (AC-2.9.13). Sits alongside Wi-Fi, volume, battery, clock icons.

**Format:** `.ico` with layers at 16×16, 20×20, 24×24, 32×32. (Tray never needs 48 or 256.)

**Critical constraint:** Must be clearly recognizable at 16×16 on both light and dark taskbar backgrounds. Windows 11 tray icons are white-on-dark by default, but users can set light taskbars. Safest approach: pure white icon with transparency, letting Windows handle contrast. Alternatively, register for theme-change notifications and swap between a dark and light variant.

**Image generator prompt concept:**

> A 16×16 pixel system tray icon of a magnifying glass, rendered as a single-weight white outline on a transparent background. Monoline stroke, minimal detail. The glass is circular, the handle is a short straight line at 45° to the lower-right. The design matches the visual weight of Windows 11 system icons (Wi-Fi, volume, Bluetooth). No fill inside the lens. No accent color — pure white (#FFFFFF) on transparent.

---

## Asset 3 — System Tray Icon, Zoomed/Active State

**Where it appears:** Same tray position as Asset 2, swapped in when zoom level > 1.0×. Swapped back to idle when zoom returns to 1.0×.

**Purpose:** Gives the user a glanceable indicator that the screen is currently magnified, without being distracting.

**Image generator prompt concept:**

> Same magnifying glass as the idle tray icon, but with a visual indicator that zoom is active. Options (generate both, pick the one that reads best at 16×16):
>
> **Option A — Filled lens:** The circular lens area is filled with white instead of hollow, making the icon appear "lit up" or active.
>
> **Option B — Plus/rays:** A small "+" sign or two small radiating lines inside the lens, suggesting magnification is engaged.
>
> **Option C — Dot accent:** A small filled circle (4×4 px at 16px scale) at the top-right corner of the icon, like a notification dot, in white.
>
> Same constraints: white on transparent, monoline, 16–32px layers.

---

## Asset 4 — Settings Window Title Bar Icon

**Where it appears:** The small icon in the top-left corner of the settings window title bar, and in the taskbar button when the settings window is open.

**Format:** Reuse the application icon (Asset 1). Windows pulls this from the `WNDCLASS` icon or the `WM_SETICON` message. No separate asset needed — just reference the same `.ico` resource at load time.

**No additional generation needed.** This is Asset 1, loaded via `LoadIcon`.

---

## Asset 5 — Installer Wizard Graphics (Inno Setup)

If using Inno Setup (per Doc 3 §5.5 and R-12 mitigation), two optional graphics dress up the installer:

### 5a — Wizard Header Banner

**Where it appears:** Top-right corner of each Inno Setup wizard page.

**Format:** BMP, 55×58 pixels (standard Inno Setup `WizardSmallImageFile`).

**Image generator prompt concept:**

> A 55×58 pixel icon of the SmoothZoom magnifying glass on a white background. Same design as the application icon but cropped tighter to the glass motif. Blue accent (#0078D4) on white. Clean, simple, no text.

### 5b — Wizard Sidebar Image

**Where it appears:** Left sidebar of the Inno Setup "Welcome" and "Finished" pages.

**Format:** BMP, 164×314 pixels (standard Inno Setup `WizardImageFile`).

**Image generator prompt concept:**

> A vertical banner graphic, 164×314 pixels. Top half: the SmoothZoom magnifying glass icon centered, rendered larger (~80px). Bottom half: the word "SmoothZoom" in a clean sans-serif font (Segoe UI weight), vertically centered below the icon. Color scheme: white background with blue accent (#0078D4) for the icon and dark gray (#1A1A1A) for the text. Minimal, professional, no decorative elements.

---

## Asset 6 — MSIX / Microsoft Store Tile Assets (if distributing via MSIX)

Only needed if the packaging format is MSIX rather than Inno Setup. These are declared in the MSIX `AppxManifest.xml`.

| Asset | Size | Purpose |
|-------|------|---------|
| Square44x44Logo | 44×44 | Taskbar, Start Menu small |
| Square44x44Logo.targetsize-* | 16, 24, 32, 48, 256 | Unplated variants for different contexts |
| Square150x150Logo | 150×150 | Start Menu medium tile |
| Square310x310Logo | 310×310 | Start Menu large tile (optional) |
| Wide310x150Logo | 310×150 | Start Menu wide tile (optional) |
| StoreLogo | 50×50 | Store listing icon |

**Image generator prompt concept (base, then resize):**

> The SmoothZoom magnifying glass icon centered on a transparent background. Blue accent (#0078D4) lens rim, light fill. Generate at 512×512 and scale down to each target size. For the plated (tile) versions, place the icon on a solid blue (#0078D4) square background with the icon rendered in white.

---

## Asset 7 — About / Splash Graphic (Optional, Phase 6 Polish)

**Where it appears:** An "About SmoothZoom" dialog accessible from the tray menu or settings window. Also usable as a README hero image or website asset.

**Format:** PNG, ~256×256 or 512×512.

**Image generator prompt concept:**

> The SmoothZoom magnifying glass icon at high resolution (512×512), with subtle depth — a very faint drop shadow or a thin outer glow in light blue. Below or beside the icon, "SmoothZoom" in Segoe UI Semibold, dark gray. Clean white or transparent background. This is the "hero" version of the brand mark — slightly more refined than the flat icon, but still aligned with Windows 11 Fluent aesthetics.

---

## Generation Checklist

| # | Asset | Sizes Needed | Format | Priority |
|---|-------|-------------|--------|----------|
| 1 | Application icon | 16, 20, 24, 32, 48, 256 | .ico (multi-layer) | **Must have** |
| 2 | Tray icon — idle | 16, 20, 24, 32 | .ico (multi-layer) | **Must have** |
| 3 | Tray icon — active | 16, 20, 24, 32 | .ico (multi-layer) | **Must have** |
| 4 | Settings window icon | (reuse Asset 1) | — | Free |
| 5a | Installer header | 55×58 | .bmp | Nice to have |
| 5b | Installer sidebar | 164×314 | .bmp | Nice to have |
| 6 | MSIX tiles | 44, 150, 310, etc. | .png (multiple) | Only if MSIX |
| 7 | About / hero graphic | 256 or 512 | .png | Nice to have |

**Total unique designs needed: 3** (app icon, tray idle, tray active). Everything else is a resize, recolor, or reformat of those three base designs.

---

## Notes for the Image Generator

- Generate all base designs at **512×512 minimum** as clean vector-style artwork on transparent backgrounds. Downscaling is easy; upscaling destroys monoline strokes.
- The 16×16 tray icons are the hardest target. After generating the base design, manually pixel-hint or redraw the 16px version to ensure every stroke lands on a full pixel. A 1.5px stroke at 512px becomes sub-pixel mush at 16px.
- Test the tray icons against both dark (#1C1C1C) and light (#F3F3F3) taskbar backgrounds.
- The `.ico` container format requires specific tools to assemble: ImageMagick `convert`, or a tool like [Greenfish Icon Editor](http://greenfishsoftware.org/gfie.php), or the `ico` crate if scripting it.
