"""Proof of concept: put a player chassis model into a level0 hangar and
render it from the side.  Uses only the existing format/render modules.

The level is drawn in the engine world frame (`level_meshes(transform=True)`),
the tank is `SOBS/TICK.sob` (the LIGHT chassis model), translated to a floor
polygon of level0.  This is not a game loop: the model is just added to the
mesh list, no rotation/entity/gameplay state is involved.
"""
import math
import os
import sys

ROOT = "/home/nooverload/gunmetal-re"
sys.path.insert(0, ROOT)
from gm import palette, render, sbw, sob            # noqa: E402

GAME = "/mnt/d/games/Gunmetal 3dfx"
OUT = os.path.join(ROOT, "out", "tank_level0")
os.makedirs(OUT, exist_ok=True)

level = os.path.join(GAME, "level0.sbw")
w = sbw.parse_file(level)
lms = render.import_lightmaps(os.path.splitext(level)[0] + ".sbc")
meshes = render.level_meshes(w, lms, transform=True)

# floor of the hangar bay at (0, -550): z = 11.5, model rests on its -17 bottom
tx, ty, floor = 0.0, -550.0, 11.5
# small yaw so the shot is not a pure orthographic profile
yaw = math.radians(12.0)
cy, sy = math.cos(yaw), math.sin(yaw)

tank = sob.parse(os.path.join(GAME, "SOBS", "TICK.sob"))
verts = [(v[0] * cy - v[1] * sy + tx, v[0] * sy + v[1] * cy + ty,
          v[2] + floor + 17.0) for v in tank.vertices]
faces = []
for f in tank.faces:
    ox, oy = f.origin[0], f.origin[1]
    origin = (ox * cy - oy * sy + tx, ox * sy + oy * cy + ty,
              f.origin[2] + floor + 17.0)
    u = (f.u[0] * cy - f.u[1] * sy, f.u[0] * sy + f.u[1] * cy, f.u[2])
    v = (f.v[0] * cy - f.v[1] * sy, f.v[0] * sy + f.v[1] * cy, f.v[2])
    n = (f.normal[0] * cy - f.normal[1] * sy,
         f.normal[0] * sy + f.normal[1] * cy, f.normal[2])
    faces.append(sob.SobFace(f.textures, f.flags, f.indices, origin, u, v, n, f.spare))
tank_mesh = sob.Sob(verts, faces, tank.name)

pool = render.TexturePool(GAME, palette.installed(GAME))
# NOTE: do not pass clip= here -- the clip test drops most of the model's
# faces (they are farther than the threshold in the model's own frame).
views = {
    "side": ((-200, -550, 72), (0, -550, 8)),
    "iso": ((175, -705, 115), (0, -550, 8)),
}
for name, (pos, look) in views.items():
    png = render.render(meshes + [tank_mesh], width=1280, height=720,
                        pos=pos, look=look, pool=pool, ambient=96,
                        mip="auto", filter="bilinear")
    path = os.path.join(OUT, "tank_%s.png" % name)
    with open(path, "wb") as fh:
        fh.write(png)
    print(path)
