#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_INCLUDE_NONE
#define NOMINMAX
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <ImGuizmo.h>
#include "ui/EditorUI.hpp"
#include <engine/editor/EditorPrefs.hpp>
#include "RenderTest.hpp"
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace engine;
using namespace engine::editor;
struct Host {
    GLFWwindow *window = nullptr;
    bool glfw = false, imgui = false, backend = false;
    ImGuiStyle base_style; // Unscaled.
    float font_scale = 1, ui_scale = 0;
    ~Host() {
        if (backend) ImGui_ImplGlfw_Shutdown();
        if (imgui) ImGui::DestroyContext();
        if (window) glfwDestroyWindow(window);
        if (glfw) glfwTerminate();
    }
    void start() {
        glfwSetErrorCallback([](int code, const char *message) { std::cerr << "GLFW " << code << ": " << message << '\n'; });
        if (!glfwInit()) throw std::runtime_error("GLFW initialization failed");
        glfw = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        // 1440x900 at the monitor's scale, kept within its work area.
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        float scale = 1, unused = 1;
        int area_x = 0, area_y = 0, area_width = 1440, area_height = 900;
        if (monitor) { glfwGetMonitorContentScale(monitor, &scale, &unused); glfwGetMonitorWorkarea(monitor, &area_x, &area_y, &area_width, &area_height); }
        const float fit = std::min({std::max(scale, 1.f), 0.9f * float(area_width) / 1440, 0.9f * float(area_height) / 900});
        const int window_width = std::max(int(1440 * fit), 640), window_height = std::max(int(900 * fit), 400);
        window = glfwCreateWindow(window_width, window_height, "GameEngine Editor", nullptr, nullptr);
        if (!window) throw std::runtime_error("Window creation failed");
        if (monitor) glfwSetWindowPos(window, area_x + std::max(area_width - window_width, 0) / 2, area_y + std::max(area_height - window_height, 0) / 2);
        glfwShowWindow(window);
        IMGUI_CHECKVERSION(); ImGui::CreateContext(); imgui = true;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        // A drag in the viewport is navigation, and must not move a floating viewport window.
        ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;
        // Number fields: drag to scrub, click to type.
        ImGui::GetIO().ConfigDragClickToInputText = true;
        // The viewport draws its own drag readout, with a background, beside the cursor.
        ImGuizmo::GetStyle().Colors[ImGuizmo::TEXT] = {0, 0, 0, 0};
        ImGuizmo::GetStyle().Colors[ImGuizmo::TEXT_SHADOW] = {0, 0, 0, 0};
        ImGui::StyleColorsDark();
        auto &style = ImGui::GetStyle();
        style.WindowRounding = 3; style.FrameRounding = 3; style.FramePadding = {6,5};
        style.Colors[ImGuiCol_WindowBg] = {0.075f,0.085f,0.105f,1};
        style.Colors[ImGuiCol_Header] = {0.16f,0.27f,0.39f,1};
        style.Colors[ImGuiCol_Button] = {0.13f,0.23f,0.34f,1};
        // Docked tabs shrink further before the bar scrolls, so the right-hand dock's four tabs all stay in sight: with
        // ImGui's 80, the Inspector's tab was scrolled out of view.
        style.TabMinWidthShrink = 40;
        base_style = style;
        // The renderer uploads the font atlas once, so the font is built for the scale the editor opens at;
        // follow_monitor() scales it on another monitor.
        font_scale = std::max(scale, 1.f);
        ImFontConfig font;
        font.SizePixels = 13 * font_scale;
        if (font_scale > 1) ImGui::GetIO().Fonts->AddFontDefaultVector(&font); else ImGui::GetIO().Fonts->AddFontDefaultBitmap(&font);
        follow_monitor();
        backend = ImGui_ImplGlfw_InitForOther(window, true);
        if (!backend) throw std::runtime_error("ImGui GLFW initialization failed");
    }
    // Between frames: sizes and text follow the content scale of the monitor the window is on.
    void follow_monitor() {
        float scale = 0, unused = 0;
        glfwGetWindowContentScale(window, &scale, &unused);
        if (scale <= 0 || scale == ui_scale) return;
        ui_scale = scale;
        auto &style = ImGui::GetStyle();
        style = base_style;
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale / font_scale;
    }
};
}
// What the session is doing, as text, for acceptance runs (--game-log): 20 lines a second of state, and once a second
// where every body is. Real-input runs read it to check results that a capture cannot show exactly.
struct StateLog {
    std::ofstream out;
    std::chrono::steady_clock::time_point last_line{}, last_bodies{};
    void write(const editor::EditorSession &session, const std::string &ui_fields) {
        if (!out.is_open()) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_line < std::chrono::milliseconds(50)) return;
        last_line = now;
        const char *state = session.play_state() == editor::PlayState::Editing ? "editing" : session.play_state() == editor::PlayState::Paused ? "paused" : "playing";
        out << "state=" << state << " tick=" << session.runtime_ticks() << " entities=" << session.scene().size() << " revision=" << session.commands().revision() << " dirty=" << (session.dirty() ? 1 : 0)
            << " selected=" << session.selected().size() << " history=" << session.history_position() << '/' << session.history_names().size() << ui_fields
            << " camera=" << session.camera.target.x << ',' << session.camera.target.y << ',' << session.camera.target.z;
        if (const auto w = session.drawn_world(session.selection())) out << " primary=" << w->matrix[12] << ',' << w->matrix[13] << ',' << w->matrix[14];
        if (auto *game = session.game()) {
            const auto hud = game->hud();
            out << " phase=" << gameplay::to_string(hud.phase) << " score=" << hud.score << " targets=" << hud.targets_remaining << " time=" << hud.time_remaining
                << " ammo=" << hud.ammo << " reserve=" << hud.reserve << " reloading=" << (hud.reloading ? 1 : 0) << " grounded=" << (game->grounded() ? 1 : 0) << " shots=" << game->shots().size();
            if (const auto w = session.scene().world_transform(game->camera())) out << " eye=" << w->matrix[12] << ',' << w->matrix[13] << ',' << w->matrix[14];
            const auto counters = game->counters();
            out << " flinches=" << counters.flinches << " fallen=" << counters.ragdolls << " footsteps=" << counters.footsteps << " shards=" << counters.shards
                << " dust=" << counters.dust << " flashes=" << counters.muzzle_flashes;
            // Targets still standing, at the middle of their first collider: an enemy's chest, not its feet.
            for (const auto id : game->standing_targets()) {
                const auto record = session.scene().inspect(id);
                const auto w = session.scene().world_transform(id);
                if (!record || !w) continue;
                std::string name = record->name;
                std::ranges::replace(name, ' ', '_');
                const auto c = record->components.colliders.empty() ? Vec3{} : record->components.colliders.front().center;
                const auto &m = w->matrix;
                out << " t:" << name << '=' << m[0] * c.x + m[4] * c.y + m[8] * c.z + m[12] << ',' << m[1] * c.x + m[5] * c.y + m[9] * c.z + m[13] << ','
                    << m[2] * c.x + m[6] * c.y + m[10] * c.z + m[14];
            }
        }
        out << '\n';
        if (session.game() && now - last_bodies >= std::chrono::seconds(1)) {
            last_bodies = now;
            out << "bodies";
            for (const auto &e : session.scene().snapshot().entities)
                if (e.components.body && e.components.body->kind == BodyKind::Dynamic) {
                    std::string name = e.name;
                    std::ranges::replace(name, ' ', '_');
                    const auto &p = e.transform.position;
                    out << ' ' << name << '=' << p.x << ',' << p.y << ',' << p.z;
                }
            out << '\n';
        }
        out.flush();
    }
};
// Frame, physics and render timings over the run, for --perf-log. Measured here, in the release loop, not estimated.
struct PerformanceLog {
    std::chrono::nanoseconds startup{0};
    std::vector<float> frame_ms;
    double render_ms_total = 0, gpu_ms_total = 0, update_ms_total = 0, build_ms_total = 0;
    std::uint64_t frames = 0, loops = 0, gpu_samples = 0;
    std::chrono::steady_clock::time_point last_drawn{};
    renderer::RendererStats last;
    int bodies = 0;
    // What the drawn frames held: particles drawn, skinned objects and animators running.
    double particles_total = 0;
    std::size_t particles_peak = 0, skinned_peak = 0, posed_peak = 0, animators_peak = 0;
    // Frames over 100 ms, a hitch anyone would see, and when the first 16 of them ended, in seconds since the log began.
    std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
    std::size_t slow_frames = 0;
    std::vector<double> slow_at_s;
    // Where the slowest frame's time went: the loops since the frame before it, which may be several when Filament skips.
    std::chrono::nanoseconds pending_update{0}, pending_build{0}, pending_render{0};
    float slowest = 0;
    double slowest_update_ms = 0, slowest_build_ms = 0, slowest_render_ms = 0;
    // Filament may skip a frame when the GPU is behind, so only frames it drew count. `update` is the session's update
    // this loop (ticks, particles, animation), `build` the UI and the frame snapshot, `took` the render call.
    void rendered(bool drawn, std::chrono::nanoseconds took, std::chrono::nanoseconds update, std::chrono::nanoseconds build,
                  const renderer::RendererStats &stats, int body_count, std::size_t animators) {
        ++loops;
        update_ms_total += std::chrono::duration<double, std::milli>(update).count();
        build_ms_total += std::chrono::duration<double, std::milli>(build).count();
        pending_update += update; pending_build += build; pending_render += took;
        if (!drawn) return;
        const auto now = std::chrono::steady_clock::now();
        if (frames++ > 10) { // Skips start-up frames.
            const float frame = std::chrono::duration<float, std::milli>(now - last_drawn).count();
            frame_ms.push_back(frame);
            if (frame > 100 && ++slow_frames <= 16) slow_at_s.push_back(std::chrono::duration<double>(now - began).count());
            if (frame > slowest) {
                const auto ms = [](std::chrono::nanoseconds t) { return std::chrono::duration<double, std::milli>(t).count(); };
                slowest = frame; slowest_update_ms = ms(pending_update); slowest_build_ms = ms(pending_build); slowest_render_ms = ms(pending_render);
            }
        }
        pending_update = pending_build = pending_render = std::chrono::nanoseconds{0};
        last_drawn = now;
        render_ms_total += std::chrono::duration<double, std::milli>(took).count();
        if (stats.gpu_frame_ms > 0) { gpu_ms_total += stats.gpu_frame_ms; ++gpu_samples; }
        particles_total += double(stats.particles);
        particles_peak = std::max(particles_peak, stats.particles);
        skinned_peak = std::max(skinned_peak, stats.skinned);
        posed_peak = std::max(posed_peak, stats.posed);
        animators_peak = std::max(animators_peak, animators);
        last = stats; bodies = body_count;
    }
    void write(const std::filesystem::path &path, const editor::SimulationTiming &timing) {
        std::ofstream out(path);
        std::sort(frame_ms.begin(), frame_ms.end());
        const auto percentile = [&](double p) { return frame_ms.empty() ? 0.f : frame_ms[std::min(std::size_t(p * double(frame_ms.size())), frame_ms.size() - 1)]; };
        const auto per_tick = [&](std::chrono::nanoseconds total) { return timing.ticks ? std::chrono::duration<double, std::milli>(total).count() / double(timing.ticks) : 0.0; };
        double total = 0;
        for (const float v : frame_ms) total += v;
        const double mean = frame_ms.empty() ? 0.0 : total / double(frame_ms.size());
        const auto ms = [](std::chrono::nanoseconds t) { return std::chrono::duration<double, std::milli>(t).count(); };
        out << "startup_ms " << ms(startup) << "\nframes_drawn " << frames << "\nloops " << loops
            << "\nframe_ms_mean " << mean << "\nframe_ms_p50 " << percentile(0.5) << "\nframe_ms_p99 " << percentile(0.99) << "\nframe_ms_max " << percentile(1.0)
            << "\nfps_mean " << (mean > 0 ? 1000.0 / mean : 0.0) << "\nfps_p99 " << (percentile(0.99) > 0 ? 1000.0 / percentile(0.99) : 0.0)
            << "\nframes_over_100ms " << slow_frames << "\nframes_over_100ms_at_s ";
        for (std::size_t i = 0; i < slow_at_s.size(); ++i) out << (i ? "," : "") << slow_at_s[i];
        if (slow_at_s.empty()) out << "none";
        out << "\nslowest_frame_update_ms " << slowest_update_ms << "\nslowest_frame_ui_and_snapshot_ms " << slowest_build_ms
            << "\nslowest_frame_render_ms " << slowest_render_ms
            << "\nupdate_ms_mean " << (loops ? update_ms_total / double(loops) : 0.0) << "\nui_and_snapshot_ms_mean " << (loops ? build_ms_total / double(loops) : 0.0)
            << "\nrender_submit_ms_mean " << (frames ? render_ms_total / double(frames) : 0.0)
            << "\ngpu_frame_ms_mean " << (gpu_samples ? gpu_ms_total / double(gpu_samples) : 0.0)
            << "\nticks " << timing.ticks << "\nphysics_step_ms_mean " << per_tick(timing.physics) << "\nphysics_step_ms_max " << ms(timing.physics_max)
            << "\ngameplay_tick_ms_mean " << per_tick(timing.gameplay)
            << "\nanimation_tick_ms_mean " << per_tick(timing.animation) << "\nanimation_tick_ms_max " << ms(timing.animation_max)
            << "\nparticle_update_ms_mean " << (timing.effect_updates ? ms(timing.effects) / double(timing.effect_updates) : 0.0) << "\nparticle_update_ms_max " << ms(timing.effects_max)
            << "\nparticles_drawn_mean " << (frames ? particles_total / double(frames) : 0.0) << "\nparticles_drawn_peak " << particles_peak
            << "\nskinned_objects_peak " << skinned_peak << "\nposed_objects_peak " << posed_peak << "\nanimators_peak " << animators_peak
            << "\nbodies " << bodies << "\nobjects " << last.objects << "\nprimitives " << last.parts << "\nlights " << last.lights
            << "\ngpu_asset_meshes " << last.meshes << "\ngpu_asset_textures " << last.textures << "\nmaterial_instances " << last.material_instances << "\nibl_builds " << last.environment_builds << '\n';
    }
};
int main(int argc, char **argv) {
    try {
        const auto program_start = std::chrono::steady_clock::now();
        bool smoke = false, open_sample_at_start = false, project_given = false;
        double perf_run_seconds = 0; // --perf-run: plays the scene, untouched, for this long, then quits.
        std::filesystem::path project = std::filesystem::current_path(), render_test_output, perf_log, game_log, start_scene, config_folder, ui_map_file, shortcuts_file;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--smoke") smoke = true;
            else if (arg == "--project" && i + 1 < argc) { project = utf8_path(argv[++i]); project_given = true; }
            else if (arg == "--sample") open_sample_at_start = true;
            else if (arg == "--render-test" && i + 1 < argc) render_test_output = utf8_path(argv[++i]);
            else if (arg == "--perf-log" && i + 1 < argc) perf_log = utf8_path(argv[++i]);
            else if (arg == "--perf-run" && i + 1 < argc) {
                perf_run_seconds = std::atof(argv[++i]);
                if (!(perf_run_seconds > 0 && perf_run_seconds <= 3600)) throw std::runtime_error("--perf-run takes seconds, above 0 and at most 3600");
            }
            else if (arg == "--game-log" && i + 1 < argc) game_log = utf8_path(argv[++i]);
            else if (arg == "--scene" && i + 1 < argc) start_scene = utf8_path(argv[++i]);
            else if (arg == "--config-dir" && i + 1 < argc) config_folder = utf8_path(argv[++i]);
            else if (arg == "--ui-map" && i + 1 < argc) ui_map_file = utf8_path(argv[++i]);
            else if (arg == "--dump-shortcuts" && i + 1 < argc) shortcuts_file = utf8_path(argv[++i]);
            else throw std::runtime_error("Usage: engine_editor [--project <folder>] [--scene <file>] [--sample] [--smoke] [--render-test <folder>] [--perf-log <file>] [--perf-run <seconds>] [--game-log <file>] [--config-dir <folder>] [--ui-map <file>] [--dump-shortcuts <file>]");
        }
        if (perf_run_seconds > 0 && (start_scene.empty() || perf_log.empty())) throw std::runtime_error("--perf-run needs --scene and --perf-log");
        if (!shortcuts_file.empty()) {
            const auto table = default_command_table();
            const auto written = write_text_file(shortcuts_file, shortcuts_markdown(table));
            if (!written) throw std::runtime_error(written.error().message);
            return 0;
        }
        // Test runs neither read nor change the user's settings.
        const bool automated = smoke || !render_test_output.empty() || perf_run_seconds > 0;
        if (!automated && config_folder.empty()) config_folder = default_config_folder();
        Host host; host.start();
        std::string layout_file;
        if (automated) ImGui::GetIO().IniFilename = nullptr;
        else {
            std::error_code error;
            std::filesystem::create_directories(config_folder, error);
            const auto utf8 = (config_folder / "layout.ini").u8string();
            layout_file.assign(reinterpret_cast<const char *>(utf8.data()), utf8.size());
            ImGui::GetIO().IniFilename = error ? nullptr : layout_file.c_str();
        }
        if (!render_test_output.empty()) return editor::run_render_test(host.window, EDITOR_MATERIAL_DIR, render_test_output);
        glfwSetDropCallback(host.window, [](GLFWwindow *, int count, const char **paths) {
            for (int i = 0; i < count; ++i) dropped_files.push_back(utf8_path(paths[i]));
        });
        editor::EditorSession session;
        // Not fatal: the editor works without a project, and imports then say why they cannot run.
        const auto project_opened = session.open_project(project);
        auto created = session.create("Cube");
        if (!created) throw std::runtime_error(created.error().message);
        session.mark_clean(); // The starter cube is not a user change.
        const auto cube = session.selection();
        renderer::DesktopRenderer renderer(glfwGetWin32Window(host.window), EDITOR_MATERIAL_DIR);
        renderer.resize_viewport(800, 600);
        GameInput game_input;
        EditorUI ui(session, renderer, game_input, host.window);
        if (!automated) { ui.config_folder = config_folder; ui.load_keymap(); ui.load_settings(); }
        ui.ui_map.set_file(ui_map_file);
        if (!project_opened) ui.log_error(project_opened.error().message);
        if (open_sample_at_start) ui.open_sample();
        if (!start_scene.empty()) {
            ui.set_scene_path(start_scene);
            ui.load_scene(start_scene, project_given);
        }
        StateLog state_log;
        if (!game_log.empty()) state_log.out.open(game_log);
        int frame = 0, rendered = 0;
        SceneSnapshot before_play;
        std::uint64_t revision_before_play = 0;
        EntityId probe = 0;
        std::array<int, 3> probe_color{}, lit_color{}, texture_top{}, texture_bottom{};
        std::string shown_title;
        auto last = std::chrono::steady_clock::now();
        PerformanceLog performance;
        std::optional<std::chrono::steady_clock::time_point> perf_run_start;
        while (!ui.quit) {
            glfwPollEvents();
            if (glfwWindowShouldClose(host.window)) {
                if (smoke) break;
                game_input.release(host.window);
                // Closing goes through the unsaved-changes prompt; restore first so a minimized window shows it.
                glfwSetWindowShouldClose(host.window, GLFW_FALSE);
                if (glfwGetWindowAttrib(host.window, GLFW_ICONIFIED)) glfwRestoreWindow(host.window);
                ui.request(Pending::Quit);
                if (ui.quit) break;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = now - last; last = now;
            // Losing the window gives the mouse back and pauses. A click on the view takes it again.
            if (game_input.captured && !glfwGetWindowAttrib(host.window, GLFW_FOCUSED)) {
                ui.free_mouse();
                if (session.game() && session.game()->phase() == gameplay::GamePhase::Playing) ui.report(session.pause(), "Paused");
            }
            // --perf-run: once the editor has drawn its first frames, Play starts and the game with it, with nobody at the
            // controls. The run ends after the time asked for; the log then holds Play alone, apart from start-up.
            if (perf_run_seconds > 0) {
                if (!perf_run_start && rendered >= 120) { // Two seconds in: the scene's models and textures are on the GPU.
                    if (const auto played = session.play(); !played) throw std::runtime_error("--perf-run could not start Play: " + played.error().message);
                    if (auto *started = session.game()) started->start();
                    const auto startup = performance.startup;
                    performance = PerformanceLog{};
                    performance.startup = startup;
                    perf_run_start = std::chrono::steady_clock::now();
                } else if (perf_run_start && now - *perf_run_start >= std::chrono::duration<double>(perf_run_seconds)) break;
            }
            auto *game = session.game();
            if (game && game_input.captured) game_input.feed(host.window, *game);
            const auto update_start = std::chrono::steady_clock::now();
            session.update(elapsed);
            const auto update_took = std::chrono::steady_clock::now() - update_start;
            if (game) game->update_view();
            if (state_log.out.is_open()) state_log.write(session, ui.log_fields());
            if (const auto title = ui.title(); title != shown_title) { glfwSetWindowTitle(host.window, title.c_str()); shown_title = title; }
            host.follow_monitor();
            int width, height; glfwGetFramebufferSize(host.window, &width, &height);
            if (width <= 0 || height <= 0) { glfwWaitEventsTimeout(0.05); continue; }
            const auto build_start = std::chrono::steady_clock::now();
            ImGui_ImplGlfw_NewFrame();
            ui.popup_before_frame = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            ui.apply_pending_layout();
            ImGui::NewFrame(); ImGuizmo::BeginFrame();
            ui.draw(); ImGui::Render();
            if (!ui.frame) { auto f = session.frame(4.0 / 3); if (f) ui.frame = f.value(); }
            const auto render_start = std::chrono::steady_clock::now();
            const bool drawn = ui.frame && renderer.render(*ui.frame, session.resources(), ImGui::GetDrawData(), width, height);
            if (drawn) { ++rendered; ui.frame_drawn(); }
            performance.rendered(drawn, std::chrono::steady_clock::now() - render_start, update_took, render_start - build_start, renderer.stats(),
                                 session.physics_bridge().diagnostics().world.body_count, session.animation().stats().animators);
            if (rendered == 1 && drawn && !perf_run_start) performance.startup = std::chrono::steady_clock::now() - program_start;
            if (!drawn && !smoke) std::this_thread::sleep_for(std::chrono::milliseconds(1)); // The GPU is behind: wait, do not spin.
            if (smoke) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                if (frame == 15) {
                    Transform t; t.position = {1.5,0.75,0}; t.scale = {1.5,1.5,1.5};
                    if (!session.execute(commands::SetTransform{cube,t})) throw std::runtime_error("Smoke transform failed");
                }
                if (frame == 25 && !session.undo()) throw std::runtime_error("Smoke undo failed");
                if (frame == 35 && !session.redo()) throw std::runtime_error("Smoke redo failed");
                if (frame == 45) {
                    if (!session.save("smoke.scene") || !session.load("smoke.scene")) throw std::runtime_error("Smoke persistence failed");
                    session.select(cube); session.focus();
                    glfwSetWindowSize(host.window, 1280, 800);
                }
                if (frame == 65) glfwIconifyWindow(host.window);
                if (frame == 65) { glfwPollEvents(); glfwRestoreWindow(host.window); }
                if (frame == 70) {
                    before_play = session.editor_scene().snapshot(); revision_before_play = session.commands().revision();
                    if (!session.play()) throw std::runtime_error("Smoke play failed");
                }
                if (frame == 72) {
                    Transform t; t.position = {-2,0.5,0};
                    if (!session.execute(commands::SetTransform{cube,t}) || !session.create("RuntimeOnly")) throw std::runtime_error("Smoke runtime edit failed");
                }
                if (frame == 80) {
                    if (!session.runtime_ticks() || renderer.object_count() != 2) throw std::runtime_error("Smoke runtime scene not ticking or not rendered");
                    if (!session.stop()) throw std::runtime_error("Smoke stop failed");
                    if (session.editor_scene().snapshot() != before_play || session.commands().revision() != revision_before_play)
                        throw std::runtime_error("Smoke play mode changed the editor scene");
                }
                // Surface color and scene lights reach the GPU: read the viewport center back and check its hue.
                // `offset` is a fraction of the viewport height below the center.
                const auto center_pixel = [&](float offset = 0) {
                    const auto pixels = renderer.readback(width, height);
                    const auto scale = ImGui::GetIO().DisplayFramebufferScale;
                    const auto x = std::clamp(int(ui.viewport_center.x * scale.x), 0, width - 1);
                    const auto y = std::clamp(int((ui.viewport_center.y + offset * ui.viewport_size.y) * scale.y), 0, height - 1);
                    const auto *p = pixels.data() + (std::size_t(y) * width + x) * 4;
                    return std::array<int, 3>{p[0], p[1], p[2]};
                };
                // Read after the display transform, which compresses ratios: 2 here is about 5 in linear light.
                const auto dominant = [](const std::array<int, 3> &c, int channel) {
                    return c[channel] > 60 && c[channel] > 2 * c[(channel + 1) % 3] && c[channel] > 2 * c[(channel + 2) % 3];
                };
                if (frame == 82) {
                    // No image-based light and no tone curve, so the hues below are the surface's and the light's alone.
                    EnvironmentComponent plain;
                    plain.source = EnvironmentSource::None; plain.tone_mapping = ToneMapping::Linear; plain.bloom = false; plain.ambient_occlusion = false; plain.shadow_quality = ShadowQuality::Off;
                    if (!session.set_environment(plain)) throw std::runtime_error("Smoke environment failed");
                    Transform t; t.position = {-3,0.5,0};
                    auto r = session.execute(commands::CreateEntity{"Probe", t, 0, 0, {MeshComponent{BuiltinMesh::Cube, {1,0,0}, 0.9, 0}, {}, {}}});
                    if (!r) throw std::runtime_error("Smoke probe creation failed");
                    probe = r.value().created.front();
                    session.select(probe); session.focus(); session.select(0); // No gizmo over the sampled pixel.
                }
                if (frame == 86 && !dominant(probe_color = center_pixel(), 0))
                    throw std::runtime_error("Smoke: red surface not visible under editor lighting");
                if (frame == 87) {
                    // White surface lit only by a blue scene light shining along the view direction.
                    const auto q = glm::quat_cast(glm::make_mat4(session.camera.world().matrix.data()));
                    Transform t; t.rotation = {q.x, q.y, q.z, q.w};
                    if (!session.execute(commands::SetMesh{probe, MeshComponent{BuiltinMesh::Cube, {1,1,1}, 0.9, 0}}) ||
                        !session.execute(commands::CreateEntity{"Blue light", t, 0, 0, {{}, {}, LightComponent{LightType::Directional, {0,0,1}, 100000, 10}}}))
                        throw std::runtime_error("Smoke component edit failed");
                }
                if (frame == 91 && (!dominant(lit_color = center_pixel(), 2) || renderer.light_count() != 1))
                    throw std::runtime_error("Smoke: scene light did not replace editor lighting");
                // An imported texture reaches the GPU the right way up: a plane seen from above, with -Z up the screen,
                // shows the image's first row (red) at the top.
                if (frame == 100) {
                    namespace fs = std::filesystem;
                    fs::remove_all("smoke-project"); fs::remove_all("smoke-source");
                    fs::create_directories("smoke-project"); fs::create_directories("smoke-source");
                    {
                        std::ofstream image("smoke-source/two_tone.ppm", std::ios::binary);
                        image << "P6\n2 2\n255\n";
                        for (const auto *pixel : {"\xff\x01\x01", "\xff\x01\x01", "\x01\x01\xff", "\x01\x01\xff"}) image.write(pixel, 3);
                    }
                    const auto opened = session.open_project("smoke-project");
                    const auto imported = session.import_asset("smoke-source/two_tone.ppm");
                    if (!opened || !imported || !session.new_scene()) throw std::runtime_error("Smoke import failed");
                    EnvironmentComponent plain;
                    plain.source = EnvironmentSource::None; plain.tone_mapping = ToneMapping::Linear; plain.bloom = false; plain.ambient_occlusion = false; plain.shadow_quality = ShadowQuality::Off;
                    if (!session.set_environment(plain)) throw std::runtime_error("Smoke environment failed");
                    Transform t; t.position = {0,0.05,0}; t.scale = {4,1,4}; // Above the grid's axis lines.
                    auto plane = session.execute(commands::CreateEntity{"Textured", t, 0, 0, {MeshComponent{BuiltinMesh::Plane, {1,1,1}, 0.9, 0}, {}, {}}});
                    if (!plane || !session.assign_asset(plane.value().created.front(), imported.value().id)) throw std::runtime_error("Smoke texture assignment failed");
                    session.select(0);
                    session.camera.target = {0,0,0}; session.camera.yaw = 0; session.camera.pitch = 1.5; session.camera.distance = 6;
                }
                if (frame == 105) {
                    texture_top = center_pixel(-0.12f); texture_bottom = center_pixel(0.12f);
                    if (!dominant(texture_top, 0) || !dominant(texture_bottom, 2) || renderer.asset_texture_count() != 1)
                        throw std::runtime_error("Smoke: imported texture not shown upright: top " + std::to_string(texture_top[0]) + "," + std::to_string(texture_top[2]) +
                                                 ", bottom " + std::to_string(texture_bottom[0]) + "," + std::to_string(texture_bottom[2]));
                }
                if (++frame == 112) {
                    const auto meshes = std::ranges::count_if(session.scene().snapshot().entities, [](const EntityRecord &e) { return e.components.mesh.has_value(); });
                    if (rendered < 20 || renderer.object_count() != std::size_t(meshes)) throw std::runtime_error("Smoke renderer did not reconcile: rendered=" + std::to_string(rendered) + " objects=" + std::to_string(renderer.object_count()));
                    renderer.capture("editor-smoke.ppm", width, height);
                    std::cout << "GPU smoke passed: " << rendered << " frames; transform, undo/redo, save/load, play/stop isolation, resize, "
                                 "surface color (" << probe_color[0] << ',' << probe_color[1] << ',' << probe_color[2] << "), scene light ("
                              << lit_color[0] << ',' << lit_color[1] << ',' << lit_color[2] << "), imported texture (top "
                              << texture_top[0] << ',' << texture_top[1] << ',' << texture_top[2] << "; bottom "
                              << texture_bottom[0] << ',' << texture_bottom[1] << ',' << texture_bottom[2] << ") and capture.\n";
                    break;
                }
            } else if (!game_input.captured && !perf_run_start) glfwWaitEventsTimeout(0.005); // A perf run plays as a captured game does: no wait.
        }
        game_input.release(host.window);
        if (!perf_log.empty()) {
            performance.write(perf_log, session.timing());
            std::ofstream imports(perf_log, std::ios::app);
            for (const auto &[path, ms] : ui.import_times) imports << "import_ms " << ms << ' ' << path << '\n';
            int width = 0, height = 0;
            glfwGetFramebufferSize(host.window, &width, &height);
            const auto scale = ImGui::GetIO().DisplayFramebufferScale;
            imports << "window " << width << 'x' << height << "\nviewport " << int(ui.viewport_size.x * scale.x) << 'x' << int(ui.viewport_size.y * scale.y) << '\n';
            if (perf_run_start)
                imports << "perf_run_s " << std::chrono::duration<double>(std::chrono::steady_clock::now() - *perf_run_start).count() << "\nphase "
                        << (session.game() ? gameplay::to_string(session.game()->phase()) : "none") << '\n';
        }
        if (smoke && frame != 112) throw std::runtime_error("Smoke run closed before completing all checks");
        std::cout << "Editor stopped cleanly.\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "Editor failed: " << e.what() << '\n'; return 1; }
}
