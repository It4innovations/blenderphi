"""Contact sheets of the Blender ANARI smoke tests: one row per test scene, one column for
the Cycles reference and one per ANARI renderer.

  python smoke_montage.py <png dir from exr_to_png.py> <output dir>
"""
import os
import sys

from PIL import Image, ImageDraw

png_root, out_root = sys.argv[1], sys.argv[2]
COLUMNS = ("reference", "cycles", "barney", "visrtx", "visrtx_quality", "mitsuba", "moonray",
           "helide", "visionaray", "visionaray_cuda", "ospray", "rpr", "photon", "photon_cpu")
TITLES = {"reference": "Blender Cycles", "cycles": "cyclesphi-anari", "visrtx": "visrtx (default)",
          "visrtx_quality": "visrtx (quality)", "visionaray": "visionaray (CPU)",
          "visionaray_cuda": "visionaray (CUDA)", "photon": "photon (CUDA)",
          "photon_cpu": "photon (CPU)"}
# Renderers without converted images are left out of the sheets.
COLUMNS = tuple(c for c in COLUMNS if os.path.isdir(os.path.join(png_root, c)))
GROUPS = {
    "basic": ("basic", "instancing", "texture", "texture_mapping"),
    "lights": ("light_sun", "light_area", "light_point", "light_spot"),
    "environment": ("environment_0", "environment_90", "background_red", "background_blue",
                    "background_strength", "background_transparent"),
    "objects": ("object_gn_instances", "object_gn_realized", "object_gn_points",
                "object_gn_curves", "object_gn_volume", "object_particle_instances",
                "object_particle_hair", "object_hair_curves", "object_text",
                "object_bevel_curve", "object_metaball", "object_subdivision"),
}
TILE = (160, 120)
LABEL_W, HEADER_H = 150, 20


def tile(path):
    if not os.path.exists(path):
        return None
    image = Image.open(path).convert("RGB")
    if image.size != TILE:
        image = image.resize(TILE, Image.LANCZOS)
    return image


os.makedirs(out_root, exist_ok=True)
for group, scenes in GROUPS.items():
    width = LABEL_W + len(COLUMNS) * TILE[0]
    height = HEADER_H + len(scenes) * TILE[1]
    sheet = Image.new("RGB", (width, height), (255, 255, 255))
    draw = ImageDraw.Draw(sheet)
    for c, column in enumerate(COLUMNS):
        draw.text((LABEL_W + c * TILE[0] + 4, 4), TITLES.get(column, column), fill=(0, 0, 0))
    for r, scene in enumerate(scenes):
        y = HEADER_H + r * TILE[1]
        draw.text((4, y + TILE[1] // 2 - 6), scene.replace("object_", ""), fill=(0, 0, 0))
        for c, column in enumerate(COLUMNS):
            x = LABEL_W + c * TILE[0]
            image = tile(os.path.join(png_root, column, scene + ".png"))
            if image is None:
                draw.rectangle([x, y, x + TILE[0] - 1, y + TILE[1] - 1], fill=(235, 235, 235))
                draw.text((x + 40, y + TILE[1] // 2 - 6), "no reference" if column == "reference"
                          else "not rendered", fill=(120, 120, 120))
            else:
                sheet.paste(image, (x, y))
    sheet.save(os.path.join(out_root, "smoke_{:s}.png".format(group)))
    print("wrote", group)
