#include "RenderTest.hpp"
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_INCLUDE_NONE
#define NOMINMAX
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <engine/editor/EditorSession.hpp>
#include <engine/renderer/DesktopRenderer.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Each check builds a small scene, renders it into the same viewport texture the editor shows, reads pixels back and
// compares them. Values are compared with each other rather than with fixed numbers, so a tone curve change does not
// invalidate them. Everything here goes through the session and frame extraction, as the editor does.
namespace engine::editor {
namespace {
namespace fs = std::filesystem;
using Color = std::array<int, 3>;
int brightness(const Color &c) { return c[0] + c[1] + c[2]; }
std::string text(const Color &c) { return std::to_string(c[0]) + "," + std::to_string(c[1]) + "," + std::to_string(c[2]); }
Transform aimed(Vec3 position, double pitch, double yaw) {
    const glm::dquat q(glm::radians(glm::dvec3(pitch, yaw, 0)));
    return {position, {q.x, q.y, q.z, q.w}, {1, 1, 1}};
}
// A TGA with 4 channels: the one uncompressed format the importers read with alpha and no decoder dependency.
void write_tga(const fs::path &path, int width, int height, const std::vector<std::array<std::uint8_t, 4>> &rows_top_first) {
    std::ofstream out(path, std::ios::binary);
    std::array<std::uint8_t, 18> header{};
    header[2] = 2;
    header[12] = std::uint8_t(width); header[13] = std::uint8_t(width >> 8);
    header[14] = std::uint8_t(height); header[15] = std::uint8_t(height >> 8);
    header[16] = 32; header[17] = 0x28; // Top-left origin, 8 alpha bits.
    out.write(reinterpret_cast<const char *>(header.data()), header.size());
    for (const auto &p : rows_top_first) { const char bgra[4]{char(p[2]), char(p[1]), char(p[0]), char(p[3])}; out.write(bgra, 4); }
}
struct Harness {
    GLFWwindow *window;
    EditorSession session;
    renderer::DesktopRenderer renderer;
    fs::path output;
    int width = 0, height = 0, failures = 0, checks = 0;
    Harness(GLFWwindow *w, const fs::path &materials, const fs::path &out) : window(w), renderer(glfwGetWin32WindowHandle(w), materials), output(out) {}
    static void *glfwGetWin32WindowHandle(GLFWwindow *w);
    // One editor-like frame: the viewport texture fills the window through ImGui.
    void frame() {
        glfwPollEvents();
        glfwGetFramebufferSize(window, &width, &height);
        ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("view", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::PopStyleVar();
        const auto size = ImGui::GetContentRegionAvail();
        renderer.resize_viewport(unsigned(size.x), unsigned(size.y));
        ImGui::Image(ImTextureRef(renderer.viewport_texture()), size, {0, 1}, {1, 0});
        ImGui::End();
        ImGui::Render();
        const auto extracted = session.frame(double(size.x) / size.y);
        if (!extracted) throw std::runtime_error("Frame failed: " + extracted.error().message);
        renderer.render(*extracted.value(), session.resources(), ImGui::GetDrawData(), width, height);
    }
    void settle() { for (int i = 0; i < 6; ++i) frame(); }
    Color pixel(float x, float y) {
        settle();
        const auto pixels = renderer.readback(width, height);
        const auto *p = pixels.data() + (std::size_t(y * float(height - 1)) * width + std::size_t(x * float(width - 1))) * 4;
        return {p[0], p[1], p[2]};
    }
    void check(bool condition, const std::string &name, const std::string &detail) {
        ++checks;
        std::cout << (condition ? "PASS  " : "FAIL  ") << name << "  [" << detail << "]\n";
        if (!condition) ++failures;
    }
    void capture(const std::string &name) { settle(); renderer.capture(output / (name + ".ppm"), width, height); }
    // An empty scene, viewed by a fixed camera, with no image-based light unless a check adds it.
    void reset() {
        if (!session.new_scene()) throw std::runtime_error("New scene failed");
        EnvironmentComponent plain;
        plain.source = EnvironmentSource::None; plain.tone_mapping = ToneMapping::Linear; plain.bloom = false; plain.ambient_occlusion = false; plain.shadow_quality = ShadowQuality::Off;
        environment(plain);
        // A scene with no lights gets the editor's two; one dark light keeps those off, so only what a check adds shines.
        LightComponent dark; dark.intensity = 0;
        add("Dark", {}, {{}, {}, dark});
        session.select(0);
        session.camera.target = {0, 0, 0}; session.camera.yaw = 0; session.camera.pitch = 0; session.camera.distance = 4;
    }
    void environment(const EnvironmentComponent &e) { if (!session.set_environment(e)) throw std::runtime_error("Environment failed"); }
    EntityId add(const std::string &name, Transform t, Components c) {
        auto r = session.execute(commands::CreateEntity{name, t, 0, 0, std::move(c)});
        if (!r) throw std::runtime_error("Create failed: " + r.error().message);
        return r.value().created.front();
    }
    EntityId sun(Vec3 direction_angles, double lux = 80000, bool shadows = false) {
        LightComponent l; l.intensity = lux; l.cast_shadows = shadows;
        return add("Sun", aimed({0, 5, 0}, direction_angles.x, direction_angles.y), {{}, {}, l});
    }
    assets::AssetId texture(const std::string &name, int w, int h, const std::vector<std::array<std::uint8_t, 4>> &pixels) {
        const fs::path source = output / "source" / (name + ".tga");
        fs::create_directories(source.parent_path());
        write_tga(source, w, h, pixels);
        auto imported = session.import_asset(source);
        if (!imported) throw std::runtime_error("Import failed: " + imported.error().message);
        return imported.value().id;
    }
    assets::MaterialTexture slot(assets::AssetId id) {
        const auto all = session.assets();
        const auto found = std::find_if(all.begin(), all.end(), [&](const assets::AssetMetadata &m) { return m.id == id; });
        return {id, found->path};
    }
    assets::AssetId material(const std::string &name, const assets::MaterialData &data) {
        auto id = session.create_material(name, data);
        if (!id) throw std::runtime_error("Material failed: " + id.error().message);
        return id.value();
    }
    // A cube filling the view's center, facing the camera, using the material.
    EntityId panel(assets::AssetId material_id, Vec3 position = {0, 0, 0}, Vec3 scale = {2, 2, 0.1}) {
        Transform t; t.position = position; t.scale = scale;
        MeshComponent m; m.color = {1, 1, 1};
        const auto id = add("Panel", t, {m, {}, {}});
        if (!session.assign_material(id, material_id)) throw std::runtime_error("Assign failed");
        return id;
    }
};
void *Harness::glfwGetWin32WindowHandle(GLFWwindow *w) { return glfwGetWin32Window(w); }

void material_factors(Harness &h) {
    h.reset(); h.sun({0, 0, 0});
    assets::MaterialData red; red.base_color = {1, 0, 0, 1}; red.roughness = 0.9;
    h.panel(h.material("red", red));
    const auto c = h.pixel(0.55f, 0.56f);
    h.check(c[0] > 60 && c[0] > 3 * c[1] && c[0] > 3 * c[2], "base color factor", text(c));
}
void sphere_faces_out(Harness &h) {
    // Lit from behind the camera: a sphere wound the wrong way would show its far, unlit inside.
    h.reset(); h.sun({0, 0, 0});
    Transform t; t.scale = {2, 2, 2};
    h.add("Ball", t, {MeshComponent{BuiltinMesh::Sphere, {1, 0, 0}, 0.9, 0}, {}, {}});
    const auto c = h.pixel(0.55f, 0.56f);
    h.check(c[0] > 120, "a sphere shows its outside", text(c));
}
void texture_color_space(Harness &h) {
    // The same gray, by value: as a linear factor 0.5 lit by the same light. The map's sRGB 188 is linear 0.5, so if the
    // base color map is read as sRGB the two panels match; read as linear the map would be far brighter.
    h.reset(); h.sun({0, 0, 0});
    const auto gray = h.texture("gray", 1, 1, {{188, 188, 188, 255}});
    assets::MaterialData mapped; mapped.base_color_map = h.slot(gray); mapped.roughness = 0.9;
    assets::MaterialData factor; factor.base_color = {0.7353, 0.7353, 0.7353, 1}; factor.roughness = 0.9; // sRGB of linear 0.5.
    const auto a = h.panel(h.material("mapped", mapped));
    const auto from_map = h.pixel(0.55f, 0.56f);
    if (!h.session.execute(commands::DeleteEntity{a})) throw std::runtime_error("Delete failed");
    h.panel(h.material("factor", factor));
    const auto from_factor = h.pixel(0.55f, 0.56f);
    h.check(std::abs(from_map[0] - from_factor[0]) <= 3, "base color map is read as sRGB", "map " + text(from_map) + ", factor " + text(from_factor));
}
void alpha_modes(Harness &h) {
    // Behind each panel a green wall. The texture's left half has alpha 0.
    h.reset(); h.sun({0, 0, 0});
    Transform wall; wall.position = {0, 0, -1}; wall.scale = {6, 6, 0.1};
    h.add("Wall", wall, {MeshComponent{BuiltinMesh::Cube, {0, 1, 0}, 0.9, 0}, {}, {}});
    const auto half = h.texture("half", 2, 1, {{255, 0, 0, 0}, {255, 0, 0, 255}});
    assets::MaterialData mask; mask.base_color_map = h.slot(half); mask.alpha_mode = assets::AlphaMode::Mask; mask.roughness = 0.9;
    const auto masked = h.panel(h.material("mask", mask));
    const auto cut = h.pixel(0.42f, 0.56f), kept = h.pixel(0.58f, 0.56f);
    h.check(cut[1] > 2 * cut[0] && kept[0] > 2 * kept[1], "alpha mask cuts transparent texels", "cut " + text(cut) + ", kept " + text(kept));
    h.capture("alpha_mask");
    if (!h.session.execute(commands::DeleteEntity{masked})) throw std::runtime_error("Delete failed");
    assets::MaterialData blend; blend.base_color = {1, 0, 0, 0.5}; blend.alpha_mode = assets::AlphaMode::Blend; blend.roughness = 0.9;
    h.panel(h.material("blend", blend));
    const auto mixed = h.pixel(0.55f, 0.56f);
    h.check(mixed[0] > 30 && mixed[1] > 30, "alpha blend shows both layers", text(mixed));
    h.capture("alpha_blend");
}
void sides(Harness &h) {
    // A plane's front faces +Y. Seen from below, a single-sided material disappears; a double-sided one stays.
    h.reset();
    h.session.camera.pitch = -1.5;
    h.sun({90, 0, 0}); // Shines up, at the plane's back.
    Transform t; t.scale = {3, 1, 3};
    assets::MaterialData one; one.base_color = {1, 0, 0, 1};
    MeshComponent m; m.mesh = BuiltinMesh::Plane; m.color = {1, 1, 1};
    const auto plane = h.add("Plane", t, {m, {}, {}});
    if (!h.session.assign_material(plane, h.material("one", one))) throw std::runtime_error("Assign failed");
    const auto culled = h.pixel(0.55f, 0.56f);
    assets::MaterialData two = one; two.double_sided = true;
    if (!h.session.assign_material(plane, h.material("two", two))) throw std::runtime_error("Assign failed");
    const auto shown = h.pixel(0.55f, 0.56f);
    // Culled, the pixel is the dark background; drawn, it is the red plane, lit from behind only if the back counts as a front.
    h.check(culled[0] <= culled[2] && shown[0] > 2 * shown[1] && shown[0] > 90, "single-sided culls, double-sided lights the back", "culled " + text(culled) + ", shown " + text(shown));
}
void normal_map(Harness &h) {
    // A light from the panel's top, grazing. A map leaning towards the image's top (green high) faces the light; one
    // leaning the other way faces away. The bitangent convention decides which is brighter.
    h.reset(); h.sun({-90, 0, 0});
    const auto up = h.texture("normal_up", 1, 1, {{128, 230, 200, 255}}), down = h.texture("normal_down", 1, 1, {{128, 26, 200, 255}});
    assets::MaterialData a; a.base_color = {0.8, 0.8, 0.8, 1}; a.roughness = 1; a.normal_map = h.slot(up);
    assets::MaterialData b = a; b.normal_map = h.slot(down);
    assets::MaterialData flat = a; flat.normal_map = {};
    const auto panel = h.panel(h.material("up", a));
    const auto toward = h.pixel(0.55f, 0.56f);
    h.session.assign_material(panel, h.material("down", b));
    const auto away = h.pixel(0.55f, 0.56f);
    h.session.assign_material(panel, h.material("flat", flat));
    const auto none = h.pixel(0.55f, 0.56f);
    h.check(brightness(toward) > brightness(none) + 40 && brightness(away) <= brightness(none), "normal map +Y leans to the image top",
            "toward " + text(toward) + ", flat " + text(none) + ", away " + text(away));
}
void metallic_roughness_map(Harness &h) {
    // Blue channel is metalness, green roughness. A smooth metal has no diffuse light: lit from the side, seen head-on,
    // it is dark where a dielectric is not. Green also matters: a rough one spreads the highlight into view.
    h.reset(); h.sun({-55, 0, 0});
    const auto dielectric = h.texture("mr_dielectric", 1, 1, {{0, 64, 0, 255}}), metal = h.texture("mr_metal", 1, 1, {{0, 64, 255, 255}});
    assets::MaterialData a; a.base_color = {1, 1, 1, 1}; a.metallic = 1; a.roughness = 1; a.metallic_roughness_map = h.slot(dielectric);
    assets::MaterialData b = a; b.metallic_roughness_map = h.slot(metal);
    const auto panel = h.panel(h.material("dielectric", a));
    const auto rough = h.pixel(0.55f, 0.56f);
    h.session.assign_material(panel, h.material("metal", b));
    const auto shiny = h.pixel(0.55f, 0.56f);
    h.check(brightness(rough) > 2 * brightness(shiny), "metallic-roughness map: blue is metalness", "dielectric " + text(rough) + ", metal " + text(shiny));
}
void occlusion_and_emissive(Harness &h) {
    // Occlusion dims the image-based light only.
    h.reset();
    EnvironmentComponent sky; sky.tone_mapping = ToneMapping::Linear; sky.bloom = false; sky.ambient_occlusion = false; sky.shadow_quality = ShadowQuality::Off; sky.show_skybox = false;
    h.environment(sky);
    const auto dark = h.texture("occluded", 1, 1, {{0, 0, 0, 255}});
    assets::MaterialData open; open.base_color = {0.8, 0.8, 0.8, 1}; open.roughness = 1;
    assets::MaterialData closed = open; closed.occlusion_map = h.slot(dark);
    const auto panel = h.panel(h.material("open", open));
    const auto lit = h.pixel(0.55f, 0.56f);
    h.session.assign_material(panel, h.material("closed", closed));
    const auto dim = h.pixel(0.55f, 0.56f);
    h.check(brightness(lit) > 3 * brightness(dim) + 10, "occlusion map dims ambient light", "open " + text(lit) + ", occluded " + text(dim));
    // Emissive shows with no light at all, tinted by its map, and does not change with exposure.
    h.reset();
    const auto red = h.texture("emissive", 1, 1, {{255, 0, 0, 255}});
    assets::MaterialData glow; glow.base_color = {0, 0, 0, 1}; glow.emissive = {1, 1, 1}; glow.emissive_map = h.slot(red);
    h.panel(h.material("glow", glow));
    const auto shining = h.pixel(0.55f, 0.56f);
    h.check(shining[0] > 200 && shining[1] < 30, "emissive map and factor, no lights", text(shining));
    auto e = h.session.environment(); e.ev100 = 5;
    h.environment(e);
    const auto other_exposure = h.pixel(0.55f, 0.56f);
    h.check(std::abs(other_exposure[0] - shining[0]) <= 3, "emissive ignores exposure", text(other_exposure));
}
void missing_texture(Harness &h) {
    // A reference to nothing: the object still draws from its factor and the problem is reported.
    h.reset(); h.sun({0, 0, 0});
    assets::MaterialData m; m.base_color = {1, 0, 0, 1}; m.roughness = 0.9; m.normal_map = {assets::AssetId{0x1234}, "gone.png"};
    h.panel(h.material("missing_map", m));
    const auto c = h.pixel(0.55f, 0.56f);
    h.check(c[0] > 60 && c[0] > 3 * c[1], "a missing map falls back to the factor", text(c));
    h.check(!h.session.asset_problems().empty(), "a missing map is reported", h.session.asset_problems().empty() ? "no problem listed" : h.session.asset_problems().front());
}
void shadows(Harness &h) {
    // A cube above a floor, sun from above. The floor under it is darker only with shadows on.
    const auto scene = [&](ShadowQuality quality) {
        h.reset();
        EnvironmentComponent e; e.source = EnvironmentSource::None; e.tone_mapping = ToneMapping::Linear; e.bloom = false; e.ambient_occlusion = false; e.shadow_quality = quality; e.shadow_distance = 20;
        h.environment(e);
        // Almost straight down, with the box 1.2 m up and the sun at 45 degrees: its shadow lands 1.2 m to the -X side.
        h.session.camera.target = {0, 0, 0}; h.session.camera.pitch = 1.5; h.session.camera.yaw = 0; h.session.camera.distance = 7;
        Transform floor; floor.scale = {12, 1, 12};
        h.add("Floor", floor, {MeshComponent{BuiltinMesh::Plane, {0.8, 0.8, 0.8}, 0.9, 0}, {}, {}});
        Transform box; box.position = {0, 1.2, 0}; box.scale = {0.8, 0.3, 0.8};
        h.add("Box", box, {MeshComponent{BuiltinMesh::Cube, {0.8, 0.8, 0.8}, 0.9, 0}, {}, {}});
        h.sun({-45, 90, 0}, 80000, true);
    };
    // x = -1.3 m and x = +2.3 m on the floor, both clear of the grid's lines.
    const float shadow_x = 0.3995f, lit_x = 0.678f, row = 0.537f;
    for (const auto quality : {ShadowQuality::Low, ShadowQuality::Medium, ShadowQuality::High}) {
        scene(quality);
        const auto beside = h.pixel(lit_x, row), under = h.pixel(shadow_x, row);
        h.check(brightness(beside) > 2 * brightness(under), "directional shadow darkens the floor, quality " + std::to_string(int(quality)), "lit " + text(beside) + ", shadowed " + text(under));
        if (quality == ShadowQuality::Medium) h.capture("shadows_on");
    }
    scene(ShadowQuality::Off);
    const auto beside_off = h.pixel(lit_x, row), under_off = h.pixel(shadow_x, row);
    h.check(std::abs(brightness(beside_off) - brightness(under_off)) < 12, "shadow quality Off removes it", "lit " + text(beside_off) + ", same spot " + text(under_off));
    // A spot light's shadow.
    h.reset();
    auto e = h.session.environment(); e.shadow_quality = ShadowQuality::Medium; e.ev100 = 6;
    h.environment(e);
    h.session.camera.target = {0, 0, 0}; h.session.camera.pitch = 1.5; h.session.camera.yaw = 0; h.session.camera.distance = 7;
    Transform floor; floor.scale = {12, 1, 12};
    h.add("Floor", floor, {MeshComponent{BuiltinMesh::Plane, {0.8, 0.8, 0.8}, 0.9, 0}, {}, {}});
    // The box is half way between the light and the floor, off to one side: its shadow lands at twice the distance.
    Transform box; box.position = {1.5, 2, 0.3}; box.scale = {0.8, 0.2, 0.8};
    const auto blocker = h.add("Box", box, {MeshComponent{BuiltinMesh::Cube, {0.8, 0.8, 0.8}, 0.9, 0}, {}, {}});
    LightComponent spot; spot.type = LightType::Spot; spot.intensity = 3000; spot.range = 12; spot.inner_cone_degrees = 50; spot.outer_cone_degrees = 60; spot.cast_shadows = true;
    const auto lamp = h.add("Spot", aimed({0, 4, 0}, -90, 0), {{}, {}, spot});
    const float shadow_u = 0.5f + 3.0f / 12.94f, mirror_u = 0.5f - 3.0f / 12.94f, row2 = 0.5f + 0.6f / 8.08f;
    const auto spot_lit = h.pixel(mirror_u, row2), spot_dark = h.pixel(shadow_u, row2);
    h.check(brightness(spot_lit) > 2 * brightness(spot_dark) && brightness(spot_lit) > 20, "spot light casts a shadow", "lit " + text(spot_lit) + ", shadowed " + text(spot_dark));
    h.capture("spot_shadow");
    auto no_shadow = spot; no_shadow.cast_shadows = false;
    if (!h.session.execute(commands::SetLight{lamp, no_shadow})) throw std::runtime_error("Light failed");
    const auto spot_open = h.pixel(shadow_u, row2);
    h.check(brightness(spot_open) > 2 * brightness(spot_dark), "turning a light's shadows off removes them", text(spot_open));
    // A point light casts too.
    LightComponent point = spot; point.type = LightType::Point;
    if (!h.session.execute(commands::SetLight{lamp, point})) throw std::runtime_error("Light failed");
    const auto point_lit = h.pixel(mirror_u, row2), point_dark = h.pixel(shadow_u, row2);
    h.check(brightness(point_lit) > 2 * brightness(point_dark) && brightness(point_lit) > 10, "point light casts a shadow", "lit " + text(point_lit) + ", shadowed " + text(point_dark));
    (void)blocker;
}
void environment_lighting(Harness &h) {
    // A panel with no direct light: black with no environment, lit by the sky, and brighter with more intensity.
    h.reset();
    assets::MaterialData m; m.base_color = {0.8, 0.8, 0.8, 1}; m.roughness = 1;
    h.panel(h.material("panel", m));
    const auto unlit = h.pixel(0.55f, 0.56f);
    EnvironmentComponent e; e.tone_mapping = ToneMapping::Linear; e.bloom = false; e.ambient_occlusion = false; e.shadow_quality = ShadowQuality::Off; e.intensity = 15000;
    h.environment(e);
    const auto lit = h.pixel(0.55f, 0.56f);
    const auto builds = h.renderer.stats().environment_builds;
    h.check(brightness(unlit) < 12 && brightness(lit) > 60, "image-based light lights an unlit surface", "off " + text(unlit) + ", on " + text(lit));
    e.intensity = 30000;
    h.environment(e);
    const auto brighter = h.pixel(0.55f, 0.56f);
    h.check(brightness(brighter) > brightness(lit), "ambient intensity scales it", text(brighter));
    h.check(h.renderer.stats().environment_builds == builds, "changing intensity does not rebuild the IBL", std::to_string(builds));
    e.ev100 += 1;
    h.environment(e);
    const auto darker = h.pixel(0.55f, 0.56f);
    h.check(brightness(darker) < brightness(brighter), "a higher EV100 darkens the image", text(darker));
    e.horizon_color = {1, 0, 0}; e.sky_color = {1, 0, 0}; e.ground_color = {1, 0, 0};
    h.environment(e);
    const auto red = h.pixel(0.55f, 0.56f);
    h.check(h.renderer.stats().environment_builds == builds + 1 && red[0] > 2 * red[1], "a new sky rebuilds the IBL and tints the surface", text(red));
    // The skybox shows behind the panel.
    e.show_skybox = true;
    h.environment(e);
    const auto sky = h.pixel(0.05f, 0.45f);
    h.check(sky[0] > 2 * sky[1], "skybox shows the environment", text(sky));
    e.show_skybox = false;
    h.environment(e);
    const auto no_sky = h.pixel(0.05f, 0.45f);
    h.check(no_sky[0] < sky[0], "skybox can be hidden", text(no_sky));
    // Procedural top differs from bottom, checking the sky's orientation on a sphere's reflection-free diffuse.
    h.reset();
    EnvironmentComponent bicolor = e; bicolor.show_skybox = true; bicolor.sky_color = {0, 0, 1}; bicolor.horizon_color = {0, 0, 0}; bicolor.ground_color = {1, 0, 0}; bicolor.intensity = 30000;
    h.environment(bicolor);
    const auto up = h.pixel(0.5f, 0.1f), down = h.pixel(0.5f, 0.9f);
    h.check(up[2] > up[0] && down[0] > down[2], "sky is above, ground below", "top " + text(up) + ", bottom " + text(down));
}
void editor_lights_follow_exposure(Harness &h) {
    // No lights in the scene: the editor's own show it the same at any exposure.
    h.reset();
    if (!h.session.new_scene()) throw std::runtime_error("New scene failed");
    EnvironmentComponent e; e.source = EnvironmentSource::None; e.tone_mapping = ToneMapping::Linear; e.bloom = false; e.ambient_occlusion = false; e.shadow_quality = ShadowQuality::Off;
    h.environment(e);
    h.session.camera.pitch = 0.4;
    Transform t; t.scale = {3, 3, 3};
    h.add("Box", t, {MeshComponent{BuiltinMesh::Cube, {0.6, 0.6, 0.6}, 0.9, 0}, {}, {}});
    const auto daylight = h.pixel(0.53f, 0.45f);
    e.ev100 = 6;
    h.environment(e);
    const auto indoor = h.pixel(0.53f, 0.45f);
    h.check(brightness(daylight) > 60 && std::abs(brightness(daylight) - brightness(indoor)) < 30, "editor lights follow the scene's exposure", "EV 15 " + text(daylight) + ", EV 6 " + text(indoor));
}
void punctual_lights(Harness &h) {
    h.reset();
    h.session.camera.pitch = 1.2; h.session.camera.distance = 8;
    Transform floor; floor.scale = {14, 1, 14};
    h.add("Floor", floor, {MeshComponent{BuiltinMesh::Plane, {0.8, 0.8, 0.8}, 0.9, 0}, {}, {}});
    auto e = h.session.environment(); e.ev100 = 6;
    h.environment(e);
    LightComponent spot; spot.type = LightType::Spot; spot.intensity = 3000; spot.range = 12; spot.inner_cone_degrees = 15; spot.outer_cone_degrees = 22;
    const auto light = h.add("Spot", aimed({0, 4, 0}, -90, 0), {{}, {}, spot});
    const auto inside = h.pixel(0.52f, 0.54f), outside = h.pixel(0.9f, 0.54f);
    h.check(brightness(inside) > 3 * brightness(outside) + 20, "spot light stays in its cone, at a real bulb's output", "inside " + text(inside) + ", outside " + text(outside));
    h.capture("spot_light");
    LightComponent point; point.type = LightType::Point; point.intensity = 3000; point.range = 12;
    if (!h.session.execute(commands::SetLight{light, point})) throw std::runtime_error("Light failed");
    const auto near_light = h.pixel(0.52f, 0.54f), far_light = h.pixel(0.97f, 0.54f);
    h.check(brightness(near_light) > brightness(far_light), "point light falls off", "near " + text(near_light) + ", far " + text(far_light));
}
void resource_lifecycle(Harness &h) {
    // Using, replacing and removing assets leaves nothing on the GPU, and unused CPU data is released.
    h.reset(); h.sun({0, 0, 0});
    const auto image = h.texture("life", 2, 2, std::vector<std::array<std::uint8_t, 4>>(4, {200, 100, 50, 255}));
    assets::MaterialData m; m.base_color_map = h.slot(image); m.normal_map = h.slot(image);
    const auto panel = h.panel(h.material("life", m));
    h.settle();
    const auto with = h.renderer.stats();
    h.check(with.textures == 1 && with.objects == 1 && with.material_instances == 1, "a material's texture reaches the GPU", std::to_string(with.textures) + " textures");
    if (!h.session.execute(commands::DeleteEntity{panel})) throw std::runtime_error("Delete failed");
    h.settle();
    const auto without = h.renderer.stats();
    h.check(without.textures == 0 && without.objects == 0 && without.material_instances == 0, "removing the object frees its GPU resources", std::to_string(without.textures) + " textures");
    const auto before_undo = h.session.undo();
    h.check(static_cast<bool>(before_undo), "undo restores the object", "");
    h.settle();
    h.check(h.renderer.stats().textures == 1, "the texture uploads again after undo", std::to_string(h.renderer.stats().textures));
}
// Particles (Milestone 5): still quads 1.5 m wide at the view's center, born in one burst and living long, so a readback
// sees them where they were born. Behind them, a self-lit blue panel shows how each blend mode combines with the scene.
void particles(Harness &h) {
    h.reset();
    h.session.view_flags.effects = true;
    const auto emitter_of = [&](const std::string &name, assets::ParticleBlend blend, std::array<double, 4> color, double strength) {
        assets::EffectData e;
        e.rate = 0; e.burst = 1; e.max_particles = 1;
        e.lifetime = {600, 600}; e.speed = {0, 0}; e.acceleration = {0, 0, 0};
        e.size = {{0, 1.5}};
        e.color = {{0, color}};
        e.blend = blend; e.emissive_strength = strength;
        const auto effect = h.session.create_effect(name, e);
        if (!effect) throw std::runtime_error("Effect failed: " + effect.error().message);
        // Centered on the row the checks read: 0.06 of the view's height below the middle, 0.28 m at this distance.
        const auto placed = h.session.place_asset_at(effect.value(), {0, -0.33, 0});
        if (!placed) throw std::runtime_error("Emitter failed: " + placed.error().message);
        h.frame();                                // Picks the emitter up ...
        h.session.update(std::chrono::milliseconds(16)); // ... and its burst.
        return placed.value().created.front();
    };
    const auto remove = [&](EntityId id) { if (!h.session.execute(commands::DeleteEntity{id})) throw std::runtime_error("Delete failed"); };
    const auto dark = h.pixel(0.5f, 0.56f);
    auto glow = emitter_of("glow", assets::ParticleBlend::Additive, {1, 0.2, 0.1, 1}, 1);
    const auto red = h.pixel(0.5f, 0.56f);
    // Additive: its light lands on the sky behind, so red rises and the other channels barely do.
    h.check(red[0] > dark[0] + 120 && red[1] < dark[1] + 25 && red[2] < dark[2] + 25, "an additive particle adds its color", "background " + text(dark) + ", particle " + text(red));
    const auto edge = h.pixel(0.5f + 0.15f * 800 / 1280, 0.56f);
    h.check(brightness(edge) < brightness(red), "the soft dot fades towards its edge", "middle " + text(red) + ", edge " + text(edge));
    h.check(h.renderer.stats().particle_batches == 1 && h.renderer.stats().particles == 1, "one batch, one particle on the GPU",
            std::to_string(h.renderer.stats().particle_batches) + " batches");
    h.capture("particles_additive");
    remove(glow);
    glow = emitter_of("dim", assets::ParticleBlend::Additive, {0.4, 0.08, 0.04, 1}, 1);
    const auto dim = h.pixel(0.5f, 0.56f);
    remove(glow);
    glow = emitter_of("bright", assets::ParticleBlend::Additive, {0.4, 0.08, 0.04, 1}, 4);
    const auto bright = h.pixel(0.5f, 0.56f);
    h.check(brightness(bright) > brightness(dim) + 40, "strength multiplies the color", "1x " + text(dim) + ", 4x " + text(bright));
    remove(glow);
    h.settle();
    h.check(h.renderer.stats().particle_batches == 0, "deleting the emitter frees its GPU batch", std::to_string(h.renderer.stats().particle_batches));
    // A blue wall that lights itself, a metre behind.
    assets::MaterialData blue; blue.base_color = {0, 0, 0, 1}; blue.emissive = {0, 0, 1}; blue.emissive_strength = 1;
    h.panel(h.material("blue_wall", blue), {0, 0, -1}, {4, 4, 0.1});
    const auto wall = h.pixel(0.5f, 0.56f);
    auto cover = emitter_of("cover", assets::ParticleBlend::Alpha, {0, 1, 0, 1}, 1);
    const auto covered = h.pixel(0.5f, 0.56f);
    h.check(covered[1] > 2 * covered[2] && covered[1] > 100, "an opaque alpha particle covers what is behind", "wall " + text(wall) + ", covered " + text(covered));
    remove(cover);
    cover = emitter_of("half", assets::ParticleBlend::Alpha, {0, 1, 0, 0.5}, 1);
    const auto half = h.pixel(0.5f, 0.56f);
    h.check(half[1] > 40 && half[2] > 40 && half[2] < wall[2], "a half alpha particle blends with what is behind", text(half));
    remove(cover);
    cover = emitter_of("add_green", assets::ParticleBlend::Additive, {0, 1, 0, 1}, 1);
    const auto added = h.pixel(0.5f, 0.56f);
    h.check(added[1] > 100 && added[2] >= wall[2] - 3, "an additive particle adds to what is behind", text(added));
    h.capture("particles_over_wall");
    h.session.view_flags.effects = false;
    h.check(h.pixel(0.5f, 0.56f)[1] < 20, "turning the preview off hides the particles", text(h.pixel(0.5f, 0.56f)));
    h.session.view_flags.effects = true;
}
void model_with_all_maps(Harness &h, const fs::path &model_file) {
    std::error_code error;
    if (!fs::is_regular_file(model_file, error)) { std::cout << "SKIP  model capture: " << model_file.string() << " not found\n"; return; }
    h.reset();
    EnvironmentComponent e; e.bloom = true; e.ambient_occlusion = true; e.shadow_quality = ShadowQuality::Medium; e.intensity = 30000;
    h.environment(e);
    LightComponent sun; sun.intensity = 60000; sun.cast_shadows = true;
    h.add("Sun", aimed({0, 5, 0}, -40, 30), {{}, {}, sun});
    const auto imported = h.session.import_asset(model_file);
    if (!imported) { h.check(false, "PBR model imports", imported.error().message); return; }
    if (!h.session.place_asset(imported.value().id)) { h.check(false, "PBR model is placed", "place failed"); return; }
    h.session.focus(); h.session.select(0);
    h.session.camera.yaw = 0.5; h.session.camera.pitch = 0.2;
    h.capture(fs::path(model_file).stem().string());
    const auto c = h.pixel(0.55f, 0.56f);
    h.check(brightness(c) > 30, "PBR model is visible", text(c));
}
// The sample's lighting test scene, through its own camera position, for visual inspection.
void lighting_scene(Harness &h, const fs::path &sample) {
    if (!h.session.open_project(sample)) { h.check(false, "lighting test scene opens", "project"); return; }
    if (const auto loaded = h.session.load(sample / "lighting_test.scene"); !loaded) { h.check(false, "lighting test scene loads", loaded.error().message); return; }
    h.session.select(0);
    h.session.camera.target = {0, 1.2, 0}; h.session.camera.yaw = 0; h.session.camera.pitch = 0.2; h.session.camera.distance = 10;
    h.capture("lighting_test");
    h.check(h.session.asset_problems().empty(), "lighting test scene resolves every asset", h.session.asset_problems().empty() ? "" : h.session.asset_problems().front());
    h.check(h.renderer.stats().textures >= 9, "its textures reach the GPU", std::to_string(h.renderer.stats().textures));
    // Rough metal against smooth dielectric, under the same light: the top row's last sphere reflects the sky; the first does not.
    const auto metal = h.pixel(0.7f, 0.33f), plain = h.pixel(0.3f, 0.33f);
    h.check(metal != plain, "metalness changes the surface", "metal " + text(metal) + ", plain " + text(plain));
}
}
int run_render_test(GLFWwindow *window, const fs::path &materials, const fs::path &output) {
    try {
        fs::remove_all(output);
        fs::create_directories(output);
        glfwSetWindowSize(window, 1280, 800);
        Harness h(window, materials, output);
        if (!h.session.open_project(output) ) throw std::runtime_error("Project failed");
        material_factors(h);
        sphere_faces_out(h);
        texture_color_space(h);
        alpha_modes(h);
        sides(h);
        normal_map(h);
        metallic_roughness_map(h);
        occlusion_and_emissive(h);
        missing_texture(h);
        shadows(h);
        environment_lighting(h);
        editor_lights_follow_exposure(h);
        punctual_lights(h);
        resource_lifecycle(h);
        particles(h);
        const fs::path sample = fs::path(ENGINE_SAMPLES_DIR) / "PhysicsShooter";
        model_with_all_maps(h, sample / "Assets/models/WaterBottle.glb");
        lighting_scene(h, sample);
        std::cout << "Render test: " << h.checks - h.failures << "/" << h.checks << " checks passed.\n";
        return h.failures ? 1 : 0;
    } catch (const std::exception &e) { std::cerr << "Render test failed: " << e.what() << '\n'; return 1; }
}
} // namespace engine::editor
