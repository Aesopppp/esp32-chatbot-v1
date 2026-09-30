"""Rebuild the 30x30 speaker icon with two curved sound waves."""

from pathlib import Path
from math import cos, radians, sin

from PIL import Image, ImageDraw


PROJECT = Path(__file__).resolve().parents[1]
SOURCE = PROJECT / "managed_components/espressif2022__esp_emote_assets/emoji_large/icon_speaker.bin"
OUTPUT_DIR = PROJECT / "assets/emoji_large"
SIZE = 30
SCALE = 12


def load_original():
    data = SOURCE.read_bytes()
    header = data[:12]
    if len(data) != 2712 or header[:8] != bytes.fromhex("190a00001e001e00"):
        raise ValueError("Unexpected source icon format; expected a 30x30 RGB565A8 GFX image")

    rgb, alpha = data[12:1812], data[1812:]
    pixels = []
    for i in range(SIZE * SIZE):
        color = (rgb[2 * i] << 8) | rgb[2 * i + 1]
        red = ((color >> 11) & 31) * 255 // 31
        green = ((color >> 5) & 63) * 255 // 63
        blue = (color & 31) * 255 // 31
        pixels.append((red, green, blue, alpha[i]))

    image = Image.new("RGBA", (SIZE, SIZE))
    image.putdata(pixels)
    return header, image


def draw_wave(draw, radius, angle, width):
    center_x, center_y = 15.0, 14.5
    points = []
    for degrees in range(-angle, angle + 1):
        theta = radians(degrees)
        points.append(
            (
                round((center_x + radius * cos(theta)) * SCALE),
                round((center_y + radius * sin(theta)) * SCALE),
            )
        )
    draw.line(points, fill=255, width=round(width * SCALE), joint="curve")


def main():
    header, original = load_original()
    wave_mask = Image.new("L", (SIZE * SCALE, SIZE * SCALE))
    draw = ImageDraw.Draw(wave_mask)
    draw_wave(draw, radius=6.2, angle=64, width=1.8)
    draw_wave(draw, radius=12.5, angle=66, width=1.8)
    wave_mask = wave_mask.resize((SIZE, SIZE), Image.Resampling.LANCZOS)

    result = Image.new("RGBA", (SIZE, SIZE))
    for y in range(SIZE):
        for x in range(SIZE):
            if x <= 16:
                result.putpixel((x, y), original.getpixel((x, y)))
            else:
                result.putpixel((x, y), (255, 255, 255, wave_mask.getpixel((x, y))))

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    result.save(OUTPUT_DIR / "icon_speaker.png")

    rgb = bytearray()
    alpha = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            red, green, blue, opacity = result.getpixel((x, y))
            color = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
            rgb.extend(color.to_bytes(2, "big"))
            alpha.append(opacity)
    (OUTPUT_DIR / "icon_speaker.bin").write_bytes(header + rgb + alpha)
    print(f"Wrote {OUTPUT_DIR / 'icon_speaker.png'} and icon_speaker.bin")


if __name__ == "__main__":
    main()
