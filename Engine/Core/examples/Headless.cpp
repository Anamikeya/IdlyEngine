#include <engine/core/Application.hpp>
#include <iostream>
using namespace engine;
using namespace engine::commands;
class Demo final : public ApplicationClient {
    Context owner_{"headless-host", 3};

  public:
    Result<void> on_start(Application &app) override {
        Command commands[] = {CreateEntity{"Root", {}, 0, 1}, CreateEntity{"Child", {}, 1, 2}};
        auto r = app.commands()->execute(owner_, 0, commands);
        if (!r)
            return r.error();
        log_info("Created two Flecs entities through the typed command bus");
        return {};
    }
    void on_stop(Application &app) noexcept override {
        if (app.scene())
            std::cout << "Stopping at tick " << app.tick() << " with " << app.scene()->size()
                      << " entities\n";
    }
};
int main() {
    Demo demo;
    Application app(demo);
    auto start = app.start();
    if (!start) {
        std::cerr << start.error().message << '\n';
        return 1;
    }
    for (int i = 0; i < 120; ++i) {
        auto r = app.advance(std::chrono::milliseconds(10));
        if (!r) {
            std::cerr << r.error().message << '\n';
            return 2;
        }
    }
    auto undo = app.commands()->undo({"headless-host", 3}, app.commands()->revision());
    if (!undo || app.scene()->size() != 0)
        return 3;
    auto redo = app.commands()->redo({"headless-host", 3}, app.commands()->revision());
    if (!redo || app.scene()->size() != 2)
        return 4;
    auto saved = write_scene(std::cout, app.scene()->snapshot());
    if (!saved)
        return 5;
    auto stopped = app.shutdown();
    return stopped ? 0 : 6;
}
