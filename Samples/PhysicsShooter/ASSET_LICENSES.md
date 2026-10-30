# Asset licenses

Every third-party file in this sample is released under **CC0 1.0** (public domain dedication). No attribution, notice or
share-alike applies, and redistribution is permitted. Authors are named as a courtesy. `asset_manifest.json` has the same
list in machine-readable form, with each file's source URL, size and SHA-256.

Checked on 2026-10-30, from each publisher's own pages. Files were downloaded from those publishers and are unmodified.

| Files in `Assets/` | Asset | Author | Source | License evidence |
|---|---|---|---|---|
| `textures/floor_base.jpg`, `floor_normal.jpg`, `floor_arm.jpg` | Concrete Floor Worn 001 | Dimitrios Savva, Rico Cilliers (Poly Haven) | https://polyhaven.com/a/concrete_floor_worn_001 | https://polyhaven.com/license: all assets CC0 |
| `textures/wall_base.jpg`, `wall_normal.jpg`, `wall_arm.jpg` | Painted Plaster Wall | Amal Kumar (Poly Haven) | https://polyhaven.com/a/painted_plaster_wall | Same |
| `textures/metal_base.jpg`, `metal_normal.jpg`, `metal_arm.jpg` | Metal Plate | Rob Tuytel (Poly Haven) | https://polyhaven.com/a/metal_plate | Same |
| `textures/wood_base.jpg`, `wood_normal.jpg`, `wood_arm.jpg` | Brown Planks 03 | Rob Tuytel (Poly Haven) | https://polyhaven.com/a/brown_planks_03 | Same |
| `environments/sky_1k.hdr` | Kloofendal 48d Partly Cloudy (Pure Sky) | Greg Zaal, Jarod Guest (Poly Haven) | https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky | Same |
| `models/WaterBottle.glb` | Water Bottle | Microsoft (Khronos glTF Sample Assets) | https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/WaterBottle | The model's `metadata.json`: `CC0-1.0`, owner "Public" |

Everything else (the `.material`, `.meta` and `.scene` files, and the gun, arena and targets built from primitives) was
written for this project by `tools/generate_project.py`.

## Size and packaging

The downloaded files total 16.6 MB, the largest being the 9.0 MB model. They are committed directly: this is small enough
for a repository with no remote, and it keeps the sample playable offline. Git LFS is installed on the development machine
but no remote exists to serve it. Move `*.glb` and `*.hdr` to LFS if a remote is added and the sample grows.

Not used: Kenney's Blaster Kit, also CC0, because its page exposes no direct download link. The gun is built from primitives instead.
Filament's own `third_party/environments` and models stay in the read-only dependency tree and are not redistributed here.
