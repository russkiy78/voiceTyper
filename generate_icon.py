#!/usr/bin/env python3
"""Generate a simple microphone icon for voiceTyper application."""

from PIL import Image, ImageDraw

def create_icon(size=256):
    """Create a microphone icon with the given size."""
    # Create image with transparent background
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    
    # Colors
    mic_body = (59, 130, 246)  # Blue
    mic_stand = (75, 85, 99)   # Gray
    mic_base = (55, 65, 81)    # Darker gray
    
    margin = size // 8
    center_x = size // 2
    
    # Mic capsule (rounded rectangle)
    capsule_width = size // 4
    capsule_height = size // 3
    capsule_top = margin
    capsule_bottom = capsule_top + capsule_height
    
    # Draw mic capsule (ellipse for rounded effect)
    draw.ellipse(
        [center_x - capsule_width, capsule_top,
         center_x + capsule_width, capsule_bottom],
        fill=mic_body
    )
    
    # Mic stand line
    stand_top = capsule_bottom
    stand_bottom = size - margin - size // 10
    stand_width = size // 20
    
    draw.rectangle(
        [center_x - stand_width, stand_top,
         center_x + stand_width, stand_bottom],
        fill=mic_stand
    )
    
    # Mic base (semicircle/arc at bottom)
    base_size = size // 3
    draw.arc(
        [center_x - base_size, stand_bottom - base_size // 2,
         center_x + base_size, stand_bottom + base_size // 2],
        start=0, end=180, fill=mic_base, width=stand_width * 2
    )
    
    return img


def main():
    sizes = [16, 32, 48, 64, 128, 256]
    
    for size in sizes:
        icon = create_icon(size)
        filename = f"voicetyper_{size}x{size}.png"
        icon.save(filename)
        print(f"Created {filename}")
    
    # Also create a standard icon.png (256x256)
    large_icon = create_icon(256)
    large_icon.save("voicetyper_icon.png")
    print("Created voicetyper_icon.png")


if __name__ == "__main__":
    main()
