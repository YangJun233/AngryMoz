from PIL import Image
src = r"C:\Users\56266\Downloads\Gemini_Generated_Image_srfky5srfky5srfk.jpg"
im = Image.open(src).convert("RGBA")
px = im.load()
w, h = im.size
# White background -> transparent, with a soft ramp on antialiased edges.
for y in range(h):
    for x in range(w):
        r, g, b, a = px[x, y]
        m = min(r, g, b)
        if m >= 248:
            px[x, y] = (r, g, b, 0)
        elif m >= 210:
            px[x, y] = (r, g, b, int(255 * (248 - m) / 38))
# Autocrop to content
bbox = im.getbbox()
im = im.crop(bbox)
w, h = im.size
# Pad to square with a small margin
side = int(max(w, h) * 1.08)
canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
canvas.paste(im, ((side - w) // 2, (side - h) // 2), im)
# Multi-size ICO from the full-res canvas
sizes = [(s, s) for s in (16, 20, 24, 32, 48, 64, 128, 256)]
canvas.save(r"D:\Work\mosquito\app.ico", sizes=sizes)
# Embed only a 256px PNG for the tray (ample for 16-32px, keeps the exe small)
small = canvas.resize((256, 256), Image.LANCZOS)
small.save(r"D:\Work\mosquito\icon.png")
print("wrote icon.png", small.size, "and app.ico", sizes)
