#include "engine/scripting/script_loader.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace engine::scripting {

ScriptLoader package_loader(const assets::VirtualFileSystem& files)
{
	return [&files](std::string_view name) -> Result<ScriptSource> {
		auto found = files.resolve(name);
		if (!found) return found.error();
		const auto& resolution = found.value();
		if (resolution.type != assets::AssetType::Script)
			return fail(ErrorCode::InvalidArgument, std::string(name) + " is a " + assets::to_string(resolution.type) + ", not a script");
		std::error_code ec;
		const auto size = std::filesystem::file_size(resolution.file, ec);
		if (ec) return fail(ErrorCode::NotFound, std::string(name) + " cannot be read: " + ec.message());
		if (size > max_script_bytes)
			return fail(ErrorCode::InvalidArgument, std::string(name) + " is larger than " + std::to_string(max_script_bytes >> 20) + " MB");
		std::ifstream in(resolution.file, std::ios::binary);
		if (!in) return fail(ErrorCode::NotFound, std::string(name) + " cannot be opened");
		ScriptSource source;
		source.name = assets::package_name(resolution.mount_point, resolution.asset_path);
		source.text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return source;
	};
}

ScriptLoader memory_loader(const std::map<std::string, std::string, std::less<>>& scripts)
{
	return [&scripts](std::string_view name) -> Result<ScriptSource> {
		const auto found = scripts.find(name);
		if (found == scripts.end()) return fail(ErrorCode::NotFound, "no script is named " + std::string(name));
		return ScriptSource{found->first, found->second};
	};
}

} // namespace engine::scripting
