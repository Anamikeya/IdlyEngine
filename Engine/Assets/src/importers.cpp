#include "engine/assets/importers.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

namespace engine::assets {
namespace {

constexpr std::size_t max_path_bytes = 1024;

char lower(char c) noexcept
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) noexcept
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

bool reserved_device_name(std::string_view segment) noexcept
{
	const std::string_view stem = segment.substr(0, segment.find('.'));
	for (const std::string_view name : {"con", "prn", "aux", "nul"}) {
		if (iequals(stem, name)) {
			return true;
		}
	}
	if (stem.size() == 4 && (iequals(stem.substr(0, 3), "com") || iequals(stem.substr(0, 3), "lpt")) && stem[3] >= '1' &&
		stem[3] <= '9') {
		return true;
	}
	return false;
}

Error parse_error(std::size_t line, std::string message)
{
	return fail(ErrorCode::ParseError, "line " + std::to_string(line) + ": " + message);
}

// Splits on spaces and tabs.
std::vector<std::string_view> tokens(std::string_view line)
{
	std::vector<std::string_view> out;
	std::size_t i = 0;
	while (i < line.size()) {
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
			++i;
		}
		const std::size_t start = i;
		while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
			++i;
		}
		if (i > start) {
			out.push_back(line.substr(start, i - start));
		}
	}
	return out;
}

bool parse_float(std::string_view text, float& out) noexcept
{
	if (!text.empty() && text.front() == '+') {
		text.remove_prefix(1);
	}
	const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
	return ec == std::errc{} && end == text.data() + text.size() && std::isfinite(out);
}

bool parse_int(std::string_view text, long long& out) noexcept
{
	const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
	return ec == std::errc{} && end == text.data() + text.size();
}

// Resolves a 1-based or negative OBJ index against `count` elements. -1 when invalid.
long long resolve_index(std::string_view text, std::size_t count) noexcept
{
	long long value = 0;
	if (!parse_int(text, value) || value == 0) {
		return -1;
	}
	const auto n = static_cast<long long>(count);
	const long long index = value > 0 ? value - 1 : n + value;
	return (index >= 0 && index < n) ? index : -1;
}

using Vec3f = std::array<float, 3>;

Vec3f sub(const Vec3f& a, const Vec3f& b) noexcept { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3f cross(const Vec3f& a, const Vec3f& b) noexcept
{
	return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vec3f normalized_or_up(Vec3f v) noexcept
{
	const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (!(len > 1.0e-20f)) {
		return {0.0f, 1.0f, 0.0f};
	}
	return {v[0] / len, v[1] / len, v[2] / len};
}

struct ByteCursor {
	std::span<const std::uint8_t> bytes;
	std::size_t offset = 0;

	[[nodiscard]] bool has(std::size_t n) const noexcept { return bytes.size() - offset >= n; }
	std::uint8_t u8() noexcept { return bytes[offset++]; }
	std::uint16_t u16() noexcept
	{
		const auto lo = bytes[offset];
		const auto hi = bytes[offset + 1];
		offset += 2;
		return static_cast<std::uint16_t>(lo | (hi << 8));
	}
};

Result<void> check_dimensions(std::uint32_t width, std::uint32_t height, const ImportLimits& limits)
{
	if (width == 0 || height == 0) {
		return fail(ErrorCode::ParseError, "texture has zero width or height");
	}
	if (width > limits.max_texture_dimension || height > limits.max_texture_dimension) {
		return fail(ErrorCode::InvalidArgument, "texture exceeds the maximum dimension");
	}
	return {};
}

} // namespace

Result<std::string> normalize_asset_path(std::string_view raw)
{
	if (raw.empty() || raw.size() > max_path_bytes) {
		return fail(ErrorCode::InvalidArgument, "asset path must be 1..1024 bytes");
	}
	std::string path(raw);
	std::replace(path.begin(), path.end(), '\\', '/');
	if (path.front() == '/') {
		return fail(ErrorCode::InvalidArgument, "asset path must be relative to the asset root");
	}
	for (const char c : path) {
		const auto u = static_cast<unsigned char>(c);
		if (u < 0x20 || u == 0x7f || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
			return fail(ErrorCode::InvalidArgument, "asset path contains a control or reserved character");
		}
	}

	std::vector<std::string_view> segments;
	std::string_view rest(path);
	while (!rest.empty()) {
		const std::size_t slash = rest.find('/');
		const std::string_view segment = rest.substr(0, slash);
		rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
		if (segment.empty() || segment == ".") {
			continue;
		}
		if (segment == "..") {
			if (segments.empty()) {
				return fail(ErrorCode::InvalidArgument, "asset path escapes the asset root");
			}
			segments.pop_back();
			continue;
		}
		if (segment.back() == '.' || segment.back() == ' ') {
			return fail(ErrorCode::InvalidArgument, "asset path segment ends with '.' or ' '");
		}
		if (reserved_device_name(segment)) {
			return fail(ErrorCode::InvalidArgument, "asset path uses a reserved device name");
		}
		segments.push_back(segment);
	}
	if (segments.empty()) {
		return fail(ErrorCode::InvalidArgument, "asset path names no file");
	}
	std::string out;
	for (const std::string_view segment : segments) {
		if (!out.empty()) {
			out += '/';
		}
		out += segment;
	}
	return out;
}

Result<MeshData> import_obj(std::string_view text, const ImportSettings& settings, const ImportLimits& limits)
{
	if (text.size() > limits.max_source_bytes) {
		return fail(ErrorCode::InvalidArgument, "OBJ source exceeds the size limit");
	}
	if (!std::isfinite(settings.mesh_scale) || !(settings.mesh_scale > 0.0f)) {
		return fail(ErrorCode::InvalidArgument, "mesh scale must be finite and positive");
	}

	std::vector<Vec3f> positions;
	std::vector<std::array<float, 2>> uvs;
	std::vector<Vec3f> normals;
	MeshData mesh;
	// (position, uv, normal) source indices, -1 for absent, to output vertex.
	std::map<std::tuple<long long, long long, long long>, std::uint32_t> unique;
	std::vector<std::uint32_t> polygon;

	std::size_t line_number = 0;
	std::string_view rest = text;
	while (!rest.empty()) {
		++line_number;
		const std::size_t newline = rest.find('\n');
		std::string_view line = rest.substr(0, newline);
		rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
			line = line.substr(0, hash);
		}
		const auto t = tokens(line);
		if (t.empty()) {
			continue;
		}
		const std::string_view keyword = t[0];

		if (keyword == "v") {
			if (t.size() != 4 && t.size() != 5) {
				return parse_error(line_number, "'v' expects 3 coordinates (optionally w)");
			}
			Vec3f p{};
			for (std::size_t i = 0; i < 3; ++i) {
				if (!parse_float(t[i + 1], p[i])) {
					return parse_error(line_number, "invalid number in 'v'");
				}
				p[i] *= settings.mesh_scale;
			}
			float w = 1.0f;
			if (t.size() == 5 && !parse_float(t[4], w)) {
				return parse_error(line_number, "invalid number in 'v'");
			}
			if (positions.size() >= limits.max_vertices) {
				return fail(ErrorCode::InvalidArgument, "OBJ exceeds the vertex limit");
			}
			positions.push_back(p);
		} else if (keyword == "vt") {
			if (t.size() < 2 || t.size() > 4) {
				return parse_error(line_number, "'vt' expects 1 to 3 coordinates");
			}
			std::array<float, 2> uv{};
			float ignored = 0.0f;
			for (std::size_t i = 1; i < t.size(); ++i) {
				if (!parse_float(t[i], i <= 2 ? uv[i - 1] : ignored)) {
					return parse_error(line_number, "invalid number in 'vt'");
				}
			}
			if (uvs.size() >= limits.max_vertices) {
				return fail(ErrorCode::InvalidArgument, "OBJ exceeds the vertex limit");
			}
			uvs.push_back({uv[0], 1.0f - uv[1]});
		} else if (keyword == "vn") {
			if (t.size() != 4) {
				return parse_error(line_number, "'vn' expects 3 components");
			}
			Vec3f n{};
			for (std::size_t i = 0; i < 3; ++i) {
				if (!parse_float(t[i + 1], n[i])) {
					return parse_error(line_number, "invalid number in 'vn'");
				}
			}
			if (normals.size() >= limits.max_vertices) {
				return fail(ErrorCode::InvalidArgument, "OBJ exceeds the vertex limit");
			}
			normals.push_back(normalized_or_up(n));
		} else if (keyword == "f") {
			if (t.size() < 4) {
				return parse_error(line_number, "'f' needs at least 3 vertices");
			}
			polygon.clear();
			for (std::size_t i = 1; i < t.size(); ++i) {
				const std::string_view ref = t[i];
				const std::size_t s1 = ref.find('/');
				const std::size_t s2 = s1 == std::string_view::npos ? std::string_view::npos : ref.find('/', s1 + 1);
				const std::string_view vp = ref.substr(0, s1);
				const std::string_view vt =
					s1 == std::string_view::npos ? std::string_view{} : ref.substr(s1 + 1, s2 == std::string_view::npos ? std::string_view::npos : s2 - s1 - 1);
				const std::string_view vn = s2 == std::string_view::npos ? std::string_view{} : ref.substr(s2 + 1);
				if (s2 != std::string_view::npos && ref.find('/', s2 + 1) != std::string_view::npos) {
					return parse_error(line_number, "malformed face vertex '" + std::string(ref) + "'");
				}
				const long long p = resolve_index(vp, positions.size());
				const long long u = vt.empty() ? -1 : resolve_index(vt, uvs.size());
				const long long n = vn.empty() ? -1 : resolve_index(vn, normals.size());
				const bool dangling_slash = (s2 != std::string_view::npos && vn.empty()) ||
					(s1 != std::string_view::npos && s2 == std::string_view::npos && vt.empty());
				if (p < 0 || (!vt.empty() && u < 0) || (!vn.empty() && n < 0) || dangling_slash) {
					return parse_error(line_number, "face vertex '" + std::string(ref) + "' references a missing element");
				}
				const auto key = std::make_tuple(p, u, n);
				auto it = unique.find(key);
				if (it == unique.end()) {
					if (mesh.vertices.size() >= limits.max_vertices) {
						return fail(ErrorCode::InvalidArgument, "OBJ exceeds the vertex limit");
					}
					MeshVertex v;
					v.position = positions[static_cast<std::size_t>(p)];
					if (u >= 0) {
						v.uv = uvs[static_cast<std::size_t>(u)];
					} else {
						mesh.missing_uvs = true;
					}
					if (n >= 0) {
						v.normal = normals[static_cast<std::size_t>(n)];
					} else {
						mesh.generated_normals = true;
					}
					it = unique.emplace(key, static_cast<std::uint32_t>(mesh.vertices.size())).first;
					mesh.vertices.push_back(v);
				}
				polygon.push_back(it->second);
			}
			if (mesh.indices.size() + 3 * (polygon.size() - 2) > limits.max_indices) {
				return fail(ErrorCode::InvalidArgument, "OBJ exceeds the index limit");
			}
			for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
				mesh.indices.insert(mesh.indices.end(), {polygon[0], polygon[i], polygon[i + 1]});
			}
		} else if (keyword == "o" || keyword == "g" || keyword == "s" || keyword == "usemtl" || keyword == "mtllib") {
			// Grouping, smoothing and material references carry no geometry; materials are out of scope.
		} else {
			return parse_error(line_number, "unsupported OBJ statement '" + std::string(keyword) + "'");
		}
	}

	if (mesh.indices.empty()) {
		return fail(ErrorCode::ParseError, "OBJ contains no faces");
	}

	if (mesh.generated_normals) {
		// Area-weighted vertex normals for the whole mesh; source normals are replaced for consistency.
		std::vector<Vec3f> accumulated(mesh.vertices.size(), Vec3f{});
		for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
			const auto& a = mesh.vertices[mesh.indices[i]].position;
			const auto& b = mesh.vertices[mesh.indices[i + 1]].position;
			const auto& c = mesh.vertices[mesh.indices[i + 2]].position;
			const Vec3f face = cross(sub(b, a), sub(c, a));
			for (std::size_t k = 0; k < 3; ++k) {
				auto& acc = accumulated[mesh.indices[i + k]];
				acc = {acc[0] + face[0], acc[1] + face[1], acc[2] + face[2]};
			}
		}
		for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
			mesh.vertices[i].normal = normalized_or_up(accumulated[i]);
		}
	}

	mesh.bounds.min = mesh.vertices.front().position;
	mesh.bounds.max = mesh.vertices.front().position;
	for (const MeshVertex& v : mesh.vertices) {
		for (std::size_t k = 0; k < 3; ++k) {
			mesh.bounds.min[k] = std::min(mesh.bounds.min[k], v.position[k]);
			mesh.bounds.max[k] = std::max(mesh.bounds.max[k], v.position[k]);
		}
	}
	return mesh;
}

Result<TextureData> import_tga(std::span<const std::uint8_t> bytes, const ImportSettings& settings, const ImportLimits& limits)
{
	if (bytes.size() > limits.max_source_bytes) {
		return fail(ErrorCode::InvalidArgument, "TGA source exceeds the size limit");
	}
	ByteCursor in{bytes};
	if (!in.has(18)) {
		return fail(ErrorCode::ParseError, "TGA header is truncated");
	}
	const std::uint8_t id_length = in.u8();
	const std::uint8_t color_map_type = in.u8();
	const std::uint8_t image_type = in.u8();
	in.offset += 5; // Colour map specification.
	in.offset += 4; // X/Y origin.
	const std::uint16_t width = in.u16();
	const std::uint16_t height = in.u16();
	const std::uint8_t bpp = in.u8();
	const std::uint8_t descriptor = in.u8();

	if (color_map_type != 0) {
		return fail(ErrorCode::ParseError, "colour-mapped TGA is not supported");
	}
	const bool rle = image_type == 10;
	const bool gray = image_type == 3;
	if (image_type != 2 && image_type != 10 && image_type != 3) {
		return fail(ErrorCode::ParseError, "TGA image type " + std::to_string(image_type) + " is not supported");
	}
	if ((gray && bpp != 8) || (!gray && bpp != 24 && bpp != 32)) {
		return fail(ErrorCode::ParseError, "unsupported TGA pixel depth " + std::to_string(bpp));
	}
	if (descriptor & 0x10) {
		return fail(ErrorCode::ParseError, "right-to-left TGA is not supported");
	}
	if (auto ok = check_dimensions(width, height, limits); !ok) {
		return ok.error();
	}
	if (!in.has(id_length)) {
		return fail(ErrorCode::ParseError, "TGA image id is truncated");
	}
	in.offset += id_length;

	const std::size_t pixel_bytes = bpp / 8u;
	const std::size_t count = static_cast<std::size_t>(width) * height;
	TextureData texture;
	texture.width = width;
	texture.height = height;
	texture.srgb = settings.texture_srgb;
	texture.pixels.resize(count * 4);

	const auto write_pixel = [&](std::size_t index, const std::uint8_t* src) {
		std::uint8_t* dst = &texture.pixels[index * 4];
		if (gray) {
			dst[0] = dst[1] = dst[2] = src[0];
			dst[3] = 255;
		} else {
			dst[0] = src[2];
			dst[1] = src[1];
			dst[2] = src[0];
			dst[3] = pixel_bytes == 4 ? src[3] : std::uint8_t{255};
		}
	};

	std::size_t written = 0;
	while (written < count) {
		if (!rle) {
			if (!in.has(count * pixel_bytes)) {
				return fail(ErrorCode::ParseError, "TGA pixel data is truncated");
			}
			for (; written < count; ++written) {
				write_pixel(written, &bytes[in.offset]);
				in.offset += pixel_bytes;
			}
			break;
		}
		if (!in.has(1)) {
			return fail(ErrorCode::ParseError, "TGA RLE data is truncated");
		}
		const std::uint8_t header = in.u8();
		const std::size_t run = (header & 0x7fu) + 1u;
		if (written + run > count) {
			return fail(ErrorCode::ParseError, "TGA RLE packet overruns the image");
		}
		if (header & 0x80u) {
			if (!in.has(pixel_bytes)) {
				return fail(ErrorCode::ParseError, "TGA RLE data is truncated");
			}
			for (std::size_t i = 0; i < run; ++i) {
				write_pixel(written++, &bytes[in.offset]);
			}
			in.offset += pixel_bytes;
		} else {
			if (!in.has(run * pixel_bytes)) {
				return fail(ErrorCode::ParseError, "TGA RLE data is truncated");
			}
			for (std::size_t i = 0; i < run; ++i) {
				write_pixel(written++, &bytes[in.offset]);
				in.offset += pixel_bytes;
			}
		}
	}

	// Bit 5 clear means the first stored row is the bottom row; output rows run top to bottom.
	if ((descriptor & 0x20) == 0) {
		const std::size_t row = static_cast<std::size_t>(width) * 4;
		for (std::size_t y = 0; y < height / 2u; ++y) {
			std::swap_ranges(texture.pixels.begin() + static_cast<std::ptrdiff_t>(y * row),
				texture.pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
				texture.pixels.begin() + static_cast<std::ptrdiff_t>((height - 1 - y) * row));
		}
	}
	return texture;
}

Result<TextureData> import_ppm(std::span<const std::uint8_t> bytes, const ImportSettings& settings, const ImportLimits& limits)
{
	if (bytes.size() > limits.max_source_bytes) {
		return fail(ErrorCode::InvalidArgument, "PPM source exceeds the size limit");
	}
	std::size_t pos = 0;
	const auto is_space = [](std::uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; };
	// Reads one header token, skipping whitespace and '#' comments.
	const auto next_token = [&](std::string& out) {
		out.clear();
		while (pos < bytes.size()) {
			if (is_space(bytes[pos])) {
				++pos;
			} else if (bytes[pos] == '#') {
				while (pos < bytes.size() && bytes[pos] != '\n') {
					++pos;
				}
			} else {
				break;
			}
		}
		while (pos < bytes.size() && !is_space(bytes[pos]) && out.size() < 16) {
			out += static_cast<char>(bytes[pos++]);
		}
		return !out.empty();
	};
	const auto to_u32 = [](const std::string& s, std::uint32_t& v) {
		const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
		return ec == std::errc{} && end == s.data() + s.size();
	};

	std::string token;
	if (!next_token(token) || token != "P6") {
		return fail(ErrorCode::ParseError, "not a binary PPM (P6) file");
	}
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t maxval = 0;
	if (!next_token(token) || !to_u32(token, width) || !next_token(token) || !to_u32(token, height) || !next_token(token) ||
		!to_u32(token, maxval)) {
		return fail(ErrorCode::ParseError, "PPM header is malformed");
	}
	if (maxval != 255) {
		return fail(ErrorCode::ParseError, "only PPM maxval 255 is supported");
	}
	if (auto ok = check_dimensions(width, height, limits); !ok) {
		return ok.error();
	}
	if (pos >= bytes.size() || !is_space(bytes[pos])) {
		return fail(ErrorCode::ParseError, "PPM header must end with one whitespace byte");
	}
	++pos;
	const std::size_t count = static_cast<std::size_t>(width) * height;
	if (bytes.size() - pos != count * 3) {
		return fail(ErrorCode::ParseError, "PPM pixel data size does not match its dimensions");
	}

	TextureData texture;
	texture.width = width;
	texture.height = height;
	texture.srgb = settings.texture_srgb;
	texture.pixels.resize(count * 4);
	for (std::size_t i = 0; i < count; ++i) {
		texture.pixels[i * 4 + 0] = bytes[pos + i * 3 + 0];
		texture.pixels[i * 4 + 1] = bytes[pos + i * 3 + 1];
		texture.pixels[i * 4 + 2] = bytes[pos + i * 3 + 2];
		texture.pixels[i * 4 + 3] = 255;
	}
	return texture;
}

Result<MetaFile> parse_meta(std::string_view text)
{
	if (text.size() > 64 * 1024) {
		return fail(ErrorCode::InvalidArgument, "meta file exceeds 64 KiB");
	}
	MetaFile meta;
	bool has_version = false;
	bool has_id = false;
	bool has_scale = false;
	bool has_srgb = false;
	std::size_t line_number = 0;
	std::string_view rest = text;
	while (!rest.empty()) {
		++line_number;
		const std::size_t newline = rest.find('\n');
		std::string_view line = rest.substr(0, newline);
		rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		const auto t = tokens(line);
		if (t.empty() || t[0].front() == '#') {
			continue;
		}
		if (t.size() != 3 || t[1] != "=") {
			return parse_error(line_number, "expected 'key = value'");
		}
		const std::string_view key = t[0];
		const std::string_view value = t[2];
		const auto once = [&](bool& seen) {
			const bool first = !seen;
			seen = true;
			return first;
		};
		if (key == "version") {
			if (!once(has_version) || value != "1") {
				return parse_error(line_number, "unsupported or duplicate version");
			}
		} else if (key == "id") {
			auto id = parse_asset_id(value);
			if (!once(has_id) || !id) {
				return parse_error(line_number, "id must be 16 hex digits, nonzero, and appear once");
			}
			meta.id = id;
		} else if (key == "mesh.scale") {
			float scale = 0.0f;
			if (!once(has_scale) || !parse_float(value, scale) || !(scale > 0.0f)) {
				return parse_error(line_number, "mesh.scale must be a positive number and appear once");
			}
			meta.settings.mesh_scale = scale;
		} else if (key == "texture.srgb") {
			if (!once(has_srgb) || (value != "true" && value != "false")) {
				return parse_error(line_number, "texture.srgb must be true or false and appear once");
			}
			meta.settings.texture_srgb = value == "true";
		} else {
			return parse_error(line_number, "unknown key '" + std::string(key) + "'");
		}
	}
	if (!has_version) {
		return fail(ErrorCode::ParseError, "meta file has no version");
	}
	return meta;
}

std::string write_meta(const MetaFile& meta)
{
	std::string out = "# GameEngine asset metadata\nversion = 1\n";
	if (meta.id) {
		out += "id = " + to_string(*meta.id) + "\n";
	}
	const ImportSettings defaults;
	if (meta.settings.mesh_scale != defaults.mesh_scale) {
		std::array<char, 32> buffer{};
		const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), meta.settings.mesh_scale);
		(void)ec;
		out += "mesh.scale = " + std::string(buffer.data(), end) + "\n";
	}
	if (meta.settings.texture_srgb != defaults.texture_srgb) {
		out += std::string("texture.srgb = ") + (meta.settings.texture_srgb ? "true" : "false") + "\n";
	}
	return out;
}

std::string to_string(AssetId id)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string out(16, '0');
	for (int i = 15; i >= 0; --i) {
		out[static_cast<std::size_t>(i)] = digits[(id.value >> (4 * (15 - i))) & 0xfu];
	}
	return out;
}

std::optional<AssetId> parse_asset_id(std::string_view text) noexcept
{
	if (text.size() != 16) {
		return std::nullopt;
	}
	std::uint64_t value = 0;
	for (const char c : text) {
		const char l = lower(c);
		std::uint64_t digit = 0;
		if (l >= '0' && l <= '9') {
			digit = static_cast<std::uint64_t>(l - '0');
		} else if (l >= 'a' && l <= 'f') {
			digit = static_cast<std::uint64_t>(l - 'a' + 10);
		} else {
			return std::nullopt;
		}
		value = (value << 4) | digit;
	}
	if (value == 0) {
		return std::nullopt;
	}
	return AssetId{value};
}

AssetId asset_id_from_path(std::string_view normalized_path) noexcept
{
	std::uint64_t hash = 0xcbf29ce484222325ull;
	for (const char c : normalized_path) {
		hash ^= static_cast<unsigned char>(c);
		hash *= 0x100000001b3ull;
	}
	return AssetId{hash == 0 ? 1 : hash};
}

const char* to_string(AssetType type) noexcept
{
	switch (type) {
	case AssetType::Unknown:
		return "unknown";
	case AssetType::Mesh:
		return "mesh";
	case AssetType::Texture:
		return "texture";
	}
	return "unknown";
}

AssetType asset_type_for(std::string_view path) noexcept
{
	const std::size_t dot = path.rfind('.');
	const std::size_t slash = path.rfind('/');
	if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
		return AssetType::Unknown;
	}
	const std::string_view ext = path.substr(dot + 1);
	if (iequals(ext, "obj") || iequals(ext, "glb")) {
		return AssetType::Mesh;
	}
	if (iequals(ext, "tga") || iequals(ext, "ppm") || iequals(ext, "png") || iequals(ext, "jpg") || iequals(ext, "jpeg")) {
		return AssetType::Texture;
	}
	return AssetType::Unknown;
}

} // namespace engine::assets
