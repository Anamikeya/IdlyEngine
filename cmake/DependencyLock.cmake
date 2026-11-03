# Audited third-party revisions. Source of truth for CI checkouts and configure-time verification.
# Update only together with docs/build/dependencies.md after re-running the audit.
# Format: <directory name under GAMEENGINE_RESOURCES_DIR> <origin url> <commit>

set(GAMEENGINE_DEPENDENCY_LOCK
	"box3d|https://github.com/erincatto/box3d|f36d061ba26998c87cb7dc593a4065aaa03fb7eb"
	"GameNetworkingSockets|https://github.com/ValveSoftware/GameNetworkingSockets|d534c19aa760df3fb75fd20db13ba1932b8a5463"
	"enet|https://github.com/lsalzman/enet|5a9c537fd464b3c6d3c55e1d3bd47588faf71b42"
	"flecs|https://github.com/SanderMertens/flecs|3988dee756aa047db1ca94e0fd61c9ad737926d8"
	"filament|https://github.com/google/filament|4e2df5e24d745a332faacccd5dd10660f232e514"
	"glfw|https://github.com/glfw/glfw|92dcf4ce74f2e2554a98fea09be7c705c17daa5a"
	"glm|https://github.com/g-truc/glm|6f14f4792a0cde5d0cf2c910506724d61cb95834"
	"imgui|https://github.com/ocornut/imgui|d88f238d1d363ef387382b2e29ad12cf96f826ac"
	"ozz-animation|https://github.com/guillaumeblanc/ozz-animation|744eb9d99f606eda849acb0b1204f7a3dc20bca1"
	"miniaudio|https://github.com/mackron/miniaudio|9634bedb5b5a2ca38c1ee7108a9358a4e233f14d"
	"spdlog|https://github.com/gabime/spdlog|6d9805df3f588402b17073dcd13eb569ceda864d"
	"tracy|https://github.com/wolfpld/tracy|1f4c53574b5913235b838d31e7d4b3c04bb2f4ab"
	"glTF-Transform|https://github.com/donmccurdy/glTF-Transform|8e2a94a2616d293c4b503d0fb6a4b42e6ec7c62f"
	"ninja|https://github.com/ninja-build/ninja|3ccb2270e8ef5a84bb31c4651c0df84440f30362"
	"vcpkg|https://github.com/microsoft/vcpkg|5363acb796e576328288fae60c25304e227e9839"
)
