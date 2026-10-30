#!/usr/bin/env python3
"""Writes the Physics Shooter sample's .meta files, materials and scenes. Run from anywhere: python -I generate_project.py

Everything it writes is committed. Rerun it after changing the layout here; the IDs come from the paths, so references
stay valid. The downloaded textures and models are not touched: see ../ASSET_LICENSES.md.
"""
import hashlib
import math
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / "Assets"
ALL_BITS = 18446744073709551615


def asset_id(path):
    return hashlib.sha1(path.encode()).hexdigest()[:16]


def ref(path):
    return f'{asset_id(path)} "{path}"' if path else "-"


def write_meta(path):
    (ASSETS / (path + ".meta")).write_text(f"version = 1\nid = {asset_id(path)}\n", newline="\n")


def number(v):
    return repr(float(v)) if v != int(v) else str(int(v))


def material(name, base=(1, 1, 1, 1), metallic=0, roughness=0.65, emissive=(0, 0, 0), strength=1, normal_scale=1,
             occlusion=1, alpha="opaque", cutoff=0.5, double_sided=0, uv=(1, 1), base_map=None, mr_map=None, normal_map=None,
             occlusion_map=None, emissive_map=None):
    path = f"materials/{name}.material"
    lines = ["ENGINE_MATERIAL 1", "base_color " + " ".join(map(number, base)), f"metallic {number(metallic)}",
             f"roughness {number(roughness)}", "emissive " + " ".join(map(number, emissive)),
             f"emissive_strength {number(strength)}", f"normal_scale {number(normal_scale)}",
             f"occlusion_strength {number(occlusion)}", f"alpha_mode {alpha}", f"alpha_cutoff {number(cutoff)}",
             f"double_sided {double_sided}", "uv_scale " + " ".join(map(number, uv)), f"base_color_map {ref(base_map)}",
             f"metallic_roughness_map {ref(mr_map)}", f"normal_map {ref(normal_map)}", f"occlusion_map {ref(occlusion_map)}",
             f"emissive_map {ref(emissive_map)}"]
    target = ASSETS / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("\n".join(lines) + "\n", newline="\n")
    write_meta(path)
    return path


def textured(name, prefix, **values):
    """A material from a Poly Haven set: the ARM map's green is roughness, blue metalness, red occlusion."""
    values.setdefault("metallic", 1)
    return material(name, roughness=1, base_map=f"textures/{prefix}_base.jpg", mr_map=f"textures/{prefix}_arm.jpg",
                    normal_map=f"textures/{prefix}_normal.jpg", occlusion_map=f"textures/{prefix}_arm.jpg", **values)


FLOOR = textured("concrete_floor", "floor")
WALL = textured("plaster_wall", "wall")
METAL = textured("metal_plate", "metal", metallic=0.3)
WOOD = textured("wood_crate", "wood")
RUBBER = material("rubber", base=(0.85, 0.3, 0.12, 1), roughness=0.45)
GUN = material("gun_metal", base=(0.16, 0.17, 0.19, 1), metallic=1, roughness=0.38)
GUN_ACCENT = material("gun_accent", base=(0.9, 0.42, 0.1, 1), roughness=0.5, emissive=(1, 0.35, 0.05), strength=0.6)
TARGET_STATIC = material("target_static", base=(0.8, 0.09, 0.07, 1), roughness=0.4, emissive=(1, 0.08, 0.05), strength=0.9)
TARGET_DYNAMIC = material("target_dynamic", base=(0.95, 0.55, 0.05, 1), roughness=0.4, emissive=(1, 0.5, 0.05), strength=0.7)
TARGET_MOVING = material("target_moving", base=(0.08, 0.4, 0.95, 1), roughness=0.3, emissive=(0.1, 0.5, 1), strength=0.9)
SPARK = material("impact_spark", base=(0, 0, 0, 1), roughness=1, emissive=(1, 0.75, 0.35), strength=7)
GLASS = material("glass", base=(0.75, 0.9, 1, 0.14), roughness=0.08, alpha="blend")
BONUS = material("bonus_zone", base=(0.25, 0.95, 0.4, 0.16), roughness=0.6, emissive=(0.1, 0.8, 0.25), strength=0.25, alpha="blend", double_sided=1)
PICKUP_TIME = material("pickup_time", base=(0.2, 0.9, 1, 1), roughness=0.2, emissive=(0.2, 0.9, 1), strength=2.5)
PICKUP_AMMO = material("pickup_ammo", base=(1, 0.9, 0.2, 1), roughness=0.2, emissive=(1, 0.85, 0.15), strength=2.5)
BOTTLE = "models/WaterBottle.glb"
SKY = "environments/sky_1k.hdr"
for file in ([f"textures/{p}_{r}.jpg" for p in ("floor", "wall", "metal", "wood") for r in ("base", "normal", "arm")] + [BOTTLE, SKY]):
    write_meta(file)


def euler(pitch=0.0, yaw=0.0):
    """Unit quaternion (x, y, z, w): pitch around X first, then yaw around Y, in degrees, as the editor's aiming does."""
    p, y = math.radians(pitch) / 2, math.radians(yaw) / 2
    return (math.sin(p) * math.cos(y), math.cos(p) * math.sin(y), -math.sin(p) * math.sin(y), math.cos(p) * math.cos(y))


class Scene:
    def __init__(self):
        self.entities = []
        self.components = []
        self.next = 1

    def add(self, name, position=(0, 0, 0), scale=(1, 1, 1), rotation=(0, 0, 0, 1), parent=0):
        id = self.next
        self.next += 1
        p, s, q = position, scale, rotation
        self.entities.append(f'ENTITY {id} {parent} "{name}" {number(p[0])} {number(p[1])} {number(p[2])} '
                             f'{q[0]!r} {q[1]!r} {q[2]!r} {q[3]!r} {number(s[0])} {number(s[1])} {number(s[2])}')
        return id

    def mesh(self, id, shape="cube", color=(1, 1, 1), mat=None, uv=(1, 1), model=None):
        self.components.append(f"MESH {id} {shape} {number(color[0])} {number(color[1])} {number(color[2])} 0.65 0 "
                               f"{ref(model)} - {ref(mat)} {number(uv[0])} {number(uv[1])}")

    def camera(self, id, fov, near, far):
        self.components.append(f"CAMERA {id} {number(fov)} {number(near)} {number(far)} 1")

    def light(self, id, kind, color, intensity, reach=10, inner=25, outer=35, shadows=0):
        self.components.append(f"LIGHT {id} {kind} {number(color[0])} {number(color[1])} {number(color[2])} {number(intensity)} "
                               f"{number(reach)} {number(inner)} {number(outer)} {shadows}")

    def body(self, id, kind, damping=0, angular_damping=0, sleep=1, bullet=0, lock=0, velocity=(0, 0, 0)):
        self.components.append(f"BODY {id} {kind} {number(velocity[0])} {number(velocity[1])} {number(velocity[2])} 0 0 0 "
                               f"{number(damping)} {number(angular_damping)} 1 {sleep} {bullet} {lock}")

    def collider(self, id, shape="box", half=(0.5, 0.5, 0.5), radius=0.5, half_height=0.5, center=(0, 0, 0), density=1000,
                 friction=0.7, restitution=0, sensor=0, events=0):
        self.components.append(f"COLLIDER {id} {shape} {number(center[0])} {number(center[1])} {number(center[2])} 0 0 0 1 "
                               f"{number(half[0])} {number(half[1])} {number(half[2])} {number(radius)} {number(half_height)} "
                               f"{number(density)} {number(friction)} {number(restitution)} {sensor} {events} {ALL_BITS} {ALL_BITS}")

    def behavior(self, id, kind, **values):
        fields = " ".join(f"{k} {number(v)}" for k, v in values.items())
        self.components.append(f"BEHAVIOR {id} {kind} {len(values)}" + (f" {fields}" if fields else ""))

    def environment(self, id, image, sky, horizon, ground, intensity, ev100, quality="medium", distance=50):
        source = "image" if image else "procedural"
        colors = " ".join(number(c) for color in (sky, horizon, ground) for c in color)
        self.components.append(f"ENVIRONMENT {id} {source} {ref(image)} {colors} {number(intensity)} 0 1 {number(ev100)} aces {quality} {number(distance)} 1 1")

    def write(self, path):
        order = {"MESH": 0, "CAMERA": 1, "LIGHT": 2, "BODY": 3, "COLLIDER": 4, "ENVIRONMENT": 5, "BEHAVIOR": 6}
        # A stable sort keeps colliders and behaviors in the order they were added.
        records = sorted(self.components, key=lambda r: (int(r.split()[1]), order[r.split()[0]]))
        text = [f"ENGINE_SCENE 6 {self.next} {len(self.entities)}"] + self.entities + [f"COMPONENTS {len(records)}"] + records
        path.write_text("\n".join(text) + "\n", newline="\n")


def solid(s, name, position, scale, mat, uv=(1, 1), friction=0.8):
    id = s.add(name, position, scale)
    s.mesh(id, mat=mat, uv=uv)
    s.collider(id, friction=friction)
    return id


def crate(s, name, position, size=1.0, density=70):
    id = s.add(name, position, (size, size, size))
    s.mesh(id, mat=WOOD)
    s.body(id, "dynamic")
    s.collider(id, density=density, friction=0.65, events=1)
    return id


def physics_shooter():
    s = Scene()
    rules = s.add("Game Rules")
    s.behavior(rules, "game_mode", time_limit=90)
    env = s.add("Environment")
    s.environment(env, SKY, (0.32, 0.5, 0.82), (0.78, 0.84, 0.92), (0.3, 0.27, 0.24), intensity=22000, ev100=14.6)
    sun = s.add("Sun", (0, 20, 0), rotation=euler(-52, 38))
    s.light(sun, "directional", (1, 0.95, 0.86), 85000, shadows=1)
    effect = s.add("Impact Effect Template", (0, -40, 0))
    s.mesh(effect, "sphere", mat=SPARK)
    s.behavior(effect, "impact_effect", lifetime=0.22, scale=0.14)

    # The arena: 32 m square, a floor and four walls.
    solid(s, "Floor", (0, -0.5, 0), (32, 1, 32), FLOOR, uv=(10, 10))
    solid(s, "Wall North", (0, 2.5, -16.5), (34, 5, 1), WALL, uv=(9, 1.4))
    solid(s, "Wall South", (0, 2.5, 16.5), (34, 5, 1), WALL, uv=(9, 1.4))
    solid(s, "Wall West", (-16.5, 2.5, 0), (1, 5, 32), WALL, uv=(9, 1.4))
    solid(s, "Wall East", (16.5, 2.5, 0), (1, 5, 32), WALL, uv=(9, 1.4))

    # The player: a capsule kept upright, with no friction so it slides along walls instead of sticking to them.
    player = s.add("Player", (0, 1.0, 12))
    s.body(player, "dynamic", sleep=0, bullet=1, lock=1)
    s.collider(player, "capsule", radius=0.35, half_height=0.55, density=130, friction=0, events=1)
    s.behavior(player, "player", walk_speed=5, sprint_speed=8.5, jump_speed=5.2, acceleration=60, air_acceleration=12, look_sensitivity=0.0022, spawn_floor=-15)
    s.behavior(player, "weapon", damage=25, fire_interval=0.12, magazine=15, reserve=150, reload_time=1.3, range=120, impulse=95)
    view = s.add("Camera", (0, 0.68, 0), parent=player)
    s.camera(view, 68, 0.05, 300)
    # The gun, in front of the camera. Its parts follow the recoil of the first.
    gun = s.add("Gun", (0.2, -0.17, -0.42), (0.05, 0.07, 0.26), parent=view)
    s.mesh(gun, mat=GUN)
    s.behavior(gun, "weapon_view", recoil=0.05, recoil_recovery=14)
    barrel = s.add("Gun Barrel", (0, 0.2, -0.75), (0.5, 0.45, 0.7), parent=gun)
    s.mesh(barrel, mat=GUN)
    grip = s.add("Gun Grip", (0, -0.75, 0.28), (0.8, 0.9, 0.3), parent=gun)
    s.mesh(grip, mat=GUN_ACCENT)

    # Cover and obstacles.
    solid(s, "Low Wall Left", (-5.5, 0.6, 7), (5, 1.2, 0.7), METAL, uv=(3, 1))
    solid(s, "Low Wall Right", (5.5, 0.6, 7), (5, 1.2, 0.7), METAL, uv=(3, 1))
    for i, (x, z) in enumerate(((-9.5, 0.5), (9.5, 0.5), (-4, -4.5), (4, -4.5))):
        solid(s, f"Pillar {i + 1}", (x, 2, z), (1.6, 4, 1.6), METAL, uv=(1, 2.5))
    # Glass stops shots until it breaks: it has health but is not a target.
    glass = s.add("Glass Barrier", (0, 1.1, 3), (5, 2.2, 0.12))
    s.mesh(glass, mat=GLASS)
    s.collider(glass)
    s.behavior(glass, "health", max=50)
    solid(s, "Platform", (11, 0.6, -9), (7, 1.2, 7), METAL, uv=(4, 4))
    solid(s, "Step", (6.8, 0.25, -11), (1.4, 0.5, 3), METAL)

    # Static targets: on the walls and a pillar. They take two shots.
    for i, position in enumerate(((-8, 3, -15.9), (8, 3, -15.9), (-9.5, 2.5, 1.38))):
        target = s.add(f"Static Target {i + 1}", position, (1.3, 1.3, 0.16))
        s.mesh(target, mat=TARGET_STATIC)
        s.collider(target)
        s.behavior(target, "health", max=50)
        s.behavior(target, "target", points=100)

    # Moving targets: kinematic bodies on back-and-forth paths.
    for i, (a, b, speed) in enumerate((((-11, 2.4, -7.8), (11, 2.4, -7.8), 3.5), ((14, 1.6, -12), (14, 1.6, 11), 3))):
        target = s.add(f"Moving Target {i + 1}", a, (1, 1, 1))
        s.mesh(target, "sphere", mat=TARGET_MOVING)
        s.body(target, "kinematic", sleep=0)
        s.collider(target, "sphere", radius=0.5)
        s.behavior(target, "health", max=50)
        s.behavior(target, "target", points=150)
        s.behavior(target, "mover", ax=a[0], ay=a[1], az=a[2], bx=b[0], by=b[1], bz=b[2], speed=speed)

    # A crate pyramid with a dynamic target on top: shooting the base topples it.
    for row, count in enumerate((4, 3, 2)):
        for i in range(count):
            crate(s, f"Crate {row}-{i}", (-11 + (i - (count - 1) / 2) * 1.03, 0.5 + row * 1.001, -9))
    target = s.add("Dynamic Target 1", (-11, 3.42, -9), (0.84, 0.84, 0.84))
    s.mesh(target, mat=TARGET_DYNAMIC)
    s.body(target, "dynamic")
    s.collider(target, density=90, friction=0.6, events=1)
    s.behavior(target, "health", max=75)
    s.behavior(target, "target", points=150)
    s.behavior(target, "impact_damage", min_speed=2.5, scale=25)

    # On the platform: a target to knock off, or to shoot.
    target = s.add("Dynamic Target 2", (11, 1.6, -9), (0.8, 0.8, 0.8))
    s.mesh(target, mat=TARGET_DYNAMIC)
    s.body(target, "dynamic")
    s.collider(target, density=60, friction=0.5, events=1)
    s.behavior(target, "health", max=75)
    s.behavior(target, "target", points=150)
    s.behavior(target, "impact_damage", min_speed=3.5, scale=25)

    # The chain reaction: one shot at the first slab topples the row, and the last falls on the target at its end.
    for i in range(10):
        slab = s.add(f"Domino {i + 1}", (6.2, 1.0, 2.6 - i * 1.0), (1.1, 2, 0.26))
        s.mesh(slab, mat=WOOD, uv=(0.5, 1))
        s.body(slab, "dynamic")
        s.collider(slab, density=240, friction=0.6, events=1)
    target = s.add("Chain Target", (6.2, 0.5, -7.6), (1, 1, 1))
    s.mesh(target, "sphere", mat=TARGET_DYNAMIC)
    s.body(target, "dynamic")
    s.collider(target, "sphere", radius=0.5, density=60, friction=0.5, events=1)
    s.behavior(target, "health", max=75)
    s.behavior(target, "target", points=200)
    s.behavior(target, "impact_damage", min_speed=1.2, scale=45)

    # A shelf of crates over a bonus zone. A crate pushed off lands in the zone, a sensor, worth points once each.
    solid(s, "Shelf Leg Left", (-2.2, 1.2, -11.5), (0.3, 2.4, 0.9), METAL)
    solid(s, "Shelf Leg Right", (2.2, 1.2, -11.5), (0.3, 2.4, 0.9), METAL)
    solid(s, "Shelf", (0, 2.5, -11.5), (5, 0.2, 1.1), METAL, uv=(3, 1))
    for i in range(5):
        crate(s, f"Shelf Crate {i + 1}", (-1.6 + i * 0.8, 2.92, -11.5), size=0.62, density=55)
    zone = s.add("Bonus Zone", (0, 1, -14), (8, 2, 2.6))
    s.mesh(zone, mat=BONUS)
    s.collider(zone, sensor=1, events=1)
    s.behavior(zone, "bonus_zone", points=50)

    # A second stack, spheres and things that fall when play starts.
    for i, (x, y, z) in enumerate(((-5, 0.4, -0.6), (-4.2, 0.4, -0.6), (-4.6, 1.2, -0.6), (-5, 0.4, -1.4), (-4.2, 0.4, -1.4), (-4.6, 1.2, -1.4))):
        crate(s, f"Stack Crate {i + 1}", (x, y, z), size=0.78, density=60)
    for i, (position, radius, restitution) in enumerate((((-1.5, 0.45, -1.5), 0.9, 0.55), ((1.5, 0.35, -1.0), 0.7, 0.8), ((-7, 7, 8), 0.8, 0.75), ((7, 9, 9), 1.0, 0.7))):
        ball = s.add(f"Ball {i + 1}", position, (radius, radius, radius))
        s.mesh(ball, "sphere", mat=RUBBER)
        s.body(ball, "dynamic", angular_damping=0.3)
        s.collider(ball, "sphere", radius=0.5, density=120, friction=0.7, restitution=restitution, events=1)

    # An imported model with every PBR map, as a target on a plinth: Khronos's Water Bottle, enlarged.
    solid(s, "Plinth", (-5, 0.5, -11), (1.2, 1, 1.2), METAL)
    bottle = s.add("Water Bottle Target", (-5, 1.63, -11), (5, 5, 5))
    s.mesh(bottle, model=BOTTLE)
    s.body(bottle, "dynamic")
    s.collider(bottle, half=(0.05, 0.128, 0.05), density=700, friction=0.6, events=1)
    s.behavior(bottle, "health", max=50)
    s.behavior(bottle, "target", points=250)
    s.behavior(bottle, "impact_damage", min_speed=4, scale=30)

    # Pickups: sensors that disappear when the player walks through them. Time is on the platform, ammo behind cover.
    time_pickup = s.add("Time Pickup", (13.2, 1.9, -11.2), (0.5, 0.5, 0.5))
    s.mesh(time_pickup, "sphere", mat=PICKUP_TIME)
    s.collider(time_pickup, "sphere", radius=0.9, sensor=1, events=1)
    s.behavior(time_pickup, "pickup", time=15)
    ammo = s.add("Ammo Pickup", (-13, 0.9, 8), (0.5, 0.5, 0.5))
    s.mesh(ammo, "sphere", mat=PICKUP_AMMO)
    s.collider(ammo, "sphere", radius=0.9, sensor=1, events=1)
    s.behavior(ammo, "pickup", ammo=45)
    s.write(ROOT / "physics_shooter.scene")


def lighting_test():
    """Metallic, rough, nonmetallic, textured and transparent surfaces under one sun and the sky."""
    s = Scene()
    env = s.add("Environment")
    s.environment(env, SKY, (0.32, 0.5, 0.82), (0.78, 0.84, 0.92), (0.3, 0.27, 0.24), intensity=22000, ev100=14.6, quality="high", distance=30)
    sun = s.add("Sun", (0, 12, 0), rotation=euler(-48, 30))
    s.light(sun, "directional", (1, 0.96, 0.9), 80000, shadows=1)
    solid(s, "Floor", (0, -0.5, 0), (24, 1, 24), FLOOR, uv=(8, 8))
    camera = s.add("Camera", (0, 2.6, 8.5), rotation=euler(-14, 0))
    s.camera(camera, 50, 0.1, 200)
    for i in range(6):
        # Top row: rising metalness. Bottom row: rising roughness, as a non-metal.
        for row, (metal, rough) in enumerate(((i / 5, 0.25), (0, 0.08 + i / 5 * 0.9))):
            name = f"m{i}" if row == 0 else f"r{i}"
            path = material(f"test_{name}", base=(0.85, 0.2, 0.12, 1) if row else (0.9, 0.78, 0.5, 1), metallic=metal, roughness=rough)
            ball = s.add(f"Sphere {name}", ((i - 2.5) * 1.5, 2.4 - row * 1.5, -1.5))
            s.mesh(ball, "sphere", mat=path)
    for i, (mat, name) in enumerate(((WOOD, "Wood"), (METAL, "Metal"), (WALL, "Plaster"))):
        cube = s.add(f"Textured {name}", (-3.2 + i * 1.8, 0.75, 2), (1.4, 1.5, 1.4))
        s.mesh(cube, mat=mat)
    s.add("Glass Panel")
    panel = s.next - 1
    s.entities[-1] = f'ENTITY {panel} 0 "Glass Panel" 3.2 1 2 0 0 0 1 2 2 0.1'
    s.mesh(panel, mat=GLASS)
    bottle = s.add("Water Bottle", (5.6, 0.65, 2), (5, 5, 5))
    s.mesh(bottle, model=BOTTLE)
    s.write(ROOT / "lighting_test.scene")


physics_shooter()
lighting_test()
print("Wrote the sample's scenes, materials and .meta files.")
