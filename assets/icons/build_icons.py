"""
Build production icon assets from source PNGs.

Usage:
    python assets/icons/build_icons.py

Requires: Pillow (pip install Pillow)

Reads from:  assets/icons/source/
Writes to:   assets/icons/production/
"""

import os
import sys
from pathlib import Path
from PIL import Image

SCRIPT_DIR = Path(__file__).parent
SOURCE_DIR = SCRIPT_DIR / "source"
OUTPUT_DIR = SCRIPT_DIR / "production"
MSIX_DIR = OUTPUT_DIR / "msix"


def crop_to_content(img, padding=0, bg_threshold=240):
    """Crop image to bounding box of non-background content.

    For RGBA images, uses alpha channel. For RGB, treats near-white as background.
    """
    if img.mode == "RGBA":
        # Use alpha channel to find content
        alpha = img.split()[3]
        bbox = alpha.getbbox()
    else:
        # For RGB, find non-white pixels
        r, g, b = img.split()
        # Create mask where any channel is below threshold
        mask = Image.new("L", img.size, 0)
        px_mask = mask.load()
        px_r, px_g, px_b = r.load(), g.load(), b.load()
        w, h = img.size
        for y in range(h):
            for x in range(w):
                if px_r[x, y] < bg_threshold or px_g[x, y] < bg_threshold or px_b[x, y] < bg_threshold:
                    px_mask[x, y] = 255
        bbox = mask.getbbox()

    if bbox is None:
        return img

    # Apply padding
    x0 = max(0, bbox[0] - padding)
    y0 = max(0, bbox[1] - padding)
    x1 = min(img.width, bbox[2] + padding)
    y1 = min(img.height, bbox[3] + padding)

    return img.crop((x0, y0, x1, y1))


def make_square(img, bg_color=(0, 0, 0, 0)):
    """Pad image to a square canvas, centered."""
    w, h = img.size
    size = max(w, h)
    if img.mode == "RGBA":
        canvas = Image.new("RGBA", (size, size), bg_color)
    else:
        canvas = Image.new("RGB", (size, size), bg_color[:3])
    offset = ((size - w) // 2, (size - h) // 2)
    canvas.paste(img, offset, img if img.mode == "RGBA" else None)
    return canvas


def build_ico(img, sizes, output_path):
    """Build multi-layer ICO file. Pillow auto-PNG-compresses 256x256 layers."""
    # Ensure RGBA
    if img.mode != "RGBA":
        img = img.convert("RGBA")

    # Ensure source is at least as large as the biggest requested size
    max_size = max(sizes)
    if img.width < max_size or img.height < max_size:
        img = img.resize((max_size, max_size), Image.LANCZOS)

    # Save as ICO — pass the large image, Pillow resizes to each requested size
    # Pillow auto-PNG-compresses layers >= 256px
    img.save(
        str(output_path),
        format="ICO",
        sizes=[(s, s) for s in sorted(sizes)],
    )


def composite_on_white(img, target_size=None):
    """Composite RGBA image onto white background, return RGB."""
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    bg = Image.new("RGB", img.size, (255, 255, 255))
    bg.paste(img, mask=img.split()[3])
    if target_size:
        bg = bg.resize(target_size, Image.LANCZOS)
    return bg


def build_app_icon():
    """Build app.ico from right half of application-icon.png (SZ monogram)."""
    src = Image.open(SOURCE_DIR / "application-icon.png")
    # Crop right half (the SZ monogram magnifier)
    w, h = src.size
    right_half = src.crop((w // 2, 0, w, h))
    # Crop to content and make square
    cropped = crop_to_content(right_half, padding=20)
    square = make_square(cropped)

    sizes = [16, 20, 24, 32, 48, 256]
    output = OUTPUT_DIR / "app.ico"
    build_ico(square, sizes, output)
    return output, sizes


def build_tray_icon(source_name, output_name):
    """Build tray .ico from source. Source already has alpha transparency."""
    src = Image.open(SOURCE_DIR / source_name)
    # Crop to content and make square
    cropped = crop_to_content(src, padding=10)
    square = make_square(cropped)

    sizes = [16, 20, 24, 32]
    output = OUTPUT_DIR / output_name
    build_ico(square, sizes, output)
    return output, sizes


def build_installer_header():
    """Build installer_header.bmp (55x58) from header banner source."""
    src = Image.open(SOURCE_DIR / "installer-wizard-header-banner.png")
    # Crop to the magnifier motif content
    cropped = crop_to_content(src, padding=10)
    # Resize to target maintaining aspect, then fit into 55x58
    target_w, target_h = 55, 58
    # Resize preserving aspect ratio to fit within target
    ratio = min(target_w / cropped.width, target_h / cropped.height)
    new_w = int(cropped.width * ratio)
    new_h = int(cropped.height * ratio)
    resized = cropped.resize((new_w, new_h), Image.LANCZOS)
    # Center on white canvas of exact target size
    canvas = Image.new("RGB", (target_w, target_h), (255, 255, 255))
    offset = ((target_w - new_w) // 2, (target_h - new_h) // 2)
    if resized.mode == "RGBA":
        canvas.paste(resized, offset, resized.split()[3])
    else:
        canvas.paste(resized, offset)

    output = OUTPUT_DIR / "installer_header.bmp"
    canvas.save(str(output), format="BMP")
    return output, (target_w, target_h)


def build_installer_sidebar():
    """Build installer_sidebar.bmp (164x314) from sidebar source."""
    src = Image.open(SOURCE_DIR / "installer-wizard-sidebar.png")
    # Source is 1024x1536 with icon+text in upper ~60%, white below
    # Scale to fit 164px wide, preserving content placement
    target_w, target_h = 164, 314

    # Scale entire image to target width
    scale = target_w / src.width
    scaled_h = int(src.height * scale)
    resized = src.resize((target_w, scaled_h), Image.LANCZOS)

    # Create white canvas and paste at top
    canvas = Image.new("RGB", (target_w, target_h), (255, 255, 255))
    # Center vertically if scaled height < target, otherwise crop from top
    if scaled_h <= target_h:
        canvas.paste(resized, (0, 0))
    else:
        # Crop to target height from top
        cropped = resized.crop((0, 0, target_w, target_h))
        canvas.paste(cropped, (0, 0))

    output = OUTPUT_DIR / "installer_sidebar.bmp"
    canvas.save(str(output), format="BMP")
    return output, (target_w, target_h)


def build_msix_tiles():
    """Build MSIX store tile PNGs from the large icon in the reference sheet."""
    src = Image.open(SOURCE_DIR / "MSIX-store-tile-base.png")
    # Large icon occupies roughly top 40% of the 1024x1536 sheet
    # Crop to the large icon area
    large_icon_region = src.crop((0, 0, src.width, int(src.height * 0.40)))
    # Crop to actual content within that region
    cropped = crop_to_content(large_icon_region, padding=10)
    square = make_square(cropped)

    tiles = {
        "Square44x44Logo.png": (44, 44),
        "Square150x150Logo.png": (150, 150),
        "Square310x310Logo.png": (310, 310),
        "Wide310x150Logo.png": (310, 150),
        "StoreLogo.png": (50, 50),
    }

    results = []
    for name, (tw, th) in tiles.items():
        output = MSIX_DIR / name
        if tw == th:
            # Square: just resize
            tile = square.resize((tw, th), Image.LANCZOS)
        else:
            # Wide: resize to fit height, center horizontally on transparent canvas
            ratio = th / square.height
            new_w = int(square.width * ratio)
            resized = square.resize((new_w, th), Image.LANCZOS)
            tile = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
            offset_x = (tw - new_w) // 2
            tile.paste(resized, (offset_x, 0), resized if resized.mode == "RGBA" else None)

        tile.save(str(output), format="PNG")
        results.append((output, (tw, th)))

    return results


def main():
    # Ensure output directories exist
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    MSIX_DIR.mkdir(parents=True, exist_ok=True)

    print("=" * 65)
    print("SmoothZoom Icon Asset Pipeline")
    print("=" * 65)
    print()

    results = []

    # 1. App icon
    print("[1/6] Building app.ico ...")
    path, sizes = build_app_icon()
    sz = os.path.getsize(path)
    results.append(("app.ico", f"ICO {len(sizes)} layers: {sizes}", sz))
    print(f"      -> {path} ({sz:,} bytes)")

    # 2. Tray idle icon
    print("[2/6] Building tray_idle.ico ...")
    path, sizes = build_tray_icon("system-tray-icon.png", "tray_idle.ico")
    sz = os.path.getsize(path)
    results.append(("tray_idle.ico", f"ICO {len(sizes)} layers: {sizes}", sz))
    print(f"      -> {path} ({sz:,} bytes)")

    # 3. Tray active icon
    print("[3/6] Building tray_active.ico ...")
    path, sizes = build_tray_icon("system-tray-icon-filled.png", "tray_active.ico")
    sz = os.path.getsize(path)
    results.append(("tray_active.ico", f"ICO {len(sizes)} layers: {sizes}", sz))
    print(f"      -> {path} ({sz:,} bytes)")

    # 4. Installer header BMP
    print("[4/6] Building installer_header.bmp ...")
    path, dims = build_installer_header()
    sz = os.path.getsize(path)
    results.append(("installer_header.bmp", f"BMP {dims[0]}x{dims[1]} 24-bit", sz))
    print(f"      -> {path} ({sz:,} bytes)")

    # 5. Installer sidebar BMP
    print("[5/6] Building installer_sidebar.bmp ...")
    path, dims = build_installer_sidebar()
    sz = os.path.getsize(path)
    results.append(("installer_sidebar.bmp", f"BMP {dims[0]}x{dims[1]} 24-bit", sz))
    print(f"      -> {path} ({sz:,} bytes)")

    # 6. MSIX tiles
    print("[6/6] Building MSIX store tiles ...")
    tile_results = build_msix_tiles()
    for path, dims in tile_results:
        sz = os.path.getsize(path)
        results.append((f"msix/{path.name}", f"PNG {dims[0]}x{dims[1]}", sz))
        print(f"      -> {path} ({sz:,} bytes)")

    # Summary table
    print()
    print("=" * 65)
    print(f"{'File':<35} {'Format':<20} {'Size':>8}")
    print("-" * 65)
    for name, fmt, size in results:
        print(f"{name:<35} {fmt:<20} {size:>7,} B")
    print("=" * 65)
    print(f"\nTotal: {len(results)} assets generated in {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
