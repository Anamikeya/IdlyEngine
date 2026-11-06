// Headless smoke test for the editor's third-party targets (contract C-6).
// Proves gameengine::imgui, gameengine::imgui_text_edit and gameengine::glfw compile, link and run under the engine's
// compiler settings.
// It opens no window and needs no GPU, so it says nothing about GUI behaviour or rendering.

#include "engine/test/test.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_null.h>
#include <TextEditor.h>

#include <string>

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

// The Script Editor's widget: Lua colors, a marker on a line, and its own undo, drawn through the null backend.
ENGINE_TEST(text_editor_shows_lua_without_a_window)
{
	ImGuiContext* context = ImGui::CreateContext();
	ENGINE_REQUIRE(context != nullptr);
	ImGui::GetIO().IniFilename = nullptr;
	ENGINE_REQUIRE(ImGui_ImplNull_Init());

	TextEditor editor;
	editor.SetLanguage(TextEditor::Language::Lua());
	ENGINE_REQUIRE(editor.GetLanguage() != nullptr);
	const std::string script = "local Gate = {}\nfunction Gate:tick(dt)\n  self.height = self.height + dt\nend\nreturn Gate\n";
	editor.SetText(script);
	editor.AddMarker(3, IM_COL32(255, 80, 80, 255), IM_COL32(255, 80, 80, 60), "Line 3", "attempt to index a nil value");

	int vertices = 0;
	for (int frame = 0; frame < 3; ++frame) {
		ImGui_ImplNull_NewFrame();
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(600, 400));
		ImGui::Begin("Script Editor");
		editor.Render("gate.lua");
		ImGui::End();
		ImGui::Render();
		vertices = ImGui::GetDrawData()->TotalVtxCount;
	}
	ENGINE_CHECK(vertices > 0);
	const std::string text = editor.GetText();
	ENGINE_CHECK_EQ(text, script);
	ENGINE_CHECK(!editor.CanUndo()); // Setting the text is not an edit.

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
