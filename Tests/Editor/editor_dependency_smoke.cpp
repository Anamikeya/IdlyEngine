// Headless smoke test for the editor's third-party targets (contract C-6).
// Proves gameengine::imgui and gameengine::glfw compile, link and run under the engine's compiler settings.
// It opens no window and needs no GPU, so it says nothing about GUI behaviour or rendering.

#include "engine/test/test.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_null.h>

ENGINE_TEST(imgui_builds_a_frame_without_a_window)
{
	IMGUI_CHECKVERSION();
	ImGuiContext* context = ImGui::CreateContext();
	ENGINE_REQUIRE(context != nullptr);
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.IniFilename = nullptr; // Never write imgui.ini from a test.
	ENGINE_REQUIRE(ImGui_ImplNull_Init());

	for (int frame = 0; frame < 3; ++frame) {
		ImGui_ImplNull_NewFrame();
		ImGui::NewFrame();
		ImGui::DockSpaceOverViewport();
		ImGui::Begin("Inspector");
		ImGui::Text("frame %d", frame);
		ImGui::End();
		ImGui::Render();
	}
	const ImDrawData* draw = ImGui::GetDrawData();
	ENGINE_REQUIRE(draw != nullptr);
	ENGINE_CHECK(draw->Valid);
	ENGINE_CHECK(draw->CmdListsCount > 0);
	ENGINE_CHECK(draw->TotalVtxCount > 0);

	ImGui_ImplNull_Shutdown();
	ImGui::DestroyContext(context);
}

ENGINE_TEST(glfw_links_without_initializing_a_display)
{
	int major = 0;
	int minor = 0;
	int revision = 0;
	glfwGetVersion(&major, &minor, &revision); // Documented as callable before glfwInit.
	ENGINE_CHECK_EQ(major, GLFW_VERSION_MAJOR);
	ENGINE_CHECK_EQ(minor, GLFW_VERSION_MINOR);
	ENGINE_CHECK(glfwGetVersionString() != nullptr);
	// The GLFW platform backend is linked into gameengine::imgui; taking its address forces the link.
	void (*new_frame)() = &ImGui_ImplGlfw_NewFrame;
	ENGINE_CHECK(new_frame != nullptr);
}
