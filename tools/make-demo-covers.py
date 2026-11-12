#!/usr/bin/env python3
"""Generates assets/covers/cover-01.png ... cover-08.png deterministically."""

from pathlib import Path

from PIL import Image, ImageDraw

PALETTES = [
    ((109, 122, 99), (178, 190, 150), (72, 84, 66)),
    ((76, 92, 112), (142, 166, 192), (42, 54, 70)),
    ((44, 46, 44), (118, 124, 120), (22, 24, 24)),
    ((111, 115, 123), (184, 188, 194), (70, 74, 82)),
    ((123, 90, 140), (190, 156, 204), (76, 52, 88)),
    ((93, 124, 90), (166, 190, 150), (54, 78, 52)),
    ((76, 92, 112), (196, 160, 120), (46, 56, 72)),
    ((176, 138, 78), (224, 196, 136), (118, 88, 42)),
]


def main() -> None:
    output = Path(__file__).resolve().parents[1] / "assets" / "covers"
    output.mkdir(parents=True, exist_ok=True)
    for index, (base, light, dark) in enumerate(PALETTES, 1):
        image = Image.new("RGB", (256, 256), base)
        draw = ImageDraw.Draw(image, "RGBA")
        for y in range(256):
            t = y / 255.0
            color = tuple(int(base[channel] * (1 - t) + light[channel] * t) for channel in range(3))
            draw.line([(0, y), (255, y)], fill=color + (255,))
        draw.ellipse((118, 28, 300, 210), fill=light + (90,))
        draw.ellipse((-70, 132, 142, 344), fill=dark + (120,))
        draw.rectangle((0, 196, 256, 256), fill=(0, 0, 0, 46))
        draw.line((40, 196, 216, 196), fill=light + (160,), width=2)
        draw.ellipse((188, 40, 208, 60), fill=(255, 248, 242, 180))
        image.save(output / f"cover-{index:02d}.png", optimize=True)


if __name__ == "__main__":
    main()
