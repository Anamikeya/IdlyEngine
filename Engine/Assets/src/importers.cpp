#include "engine/assets/importers.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <span>
#include <string>
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
	compute_tangents(mesh, 0, static_cast<std::uint32_t>(mesh.indices.size()));

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

namespace {

std::vector<std::string> line_tokens(std::string_view line, bool& valid);

bool event_name(std::string_view name) noexcept
{
	return !name.empty() && name.size() <= 32 &&
		std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool clip_name(std::string_view name) noexcept
{
	return !name.empty() && name.size() <= 256 &&
		std::none_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 32 || c == 127; });
}

std::string shortest(float value)
{
	std::array<char, 32> buffer{};
	const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
	(void)ec;
	return std::string(buffer.data(), end);
}

} // namespace

Result<std::vector<ClipEvent>> normalize_clip_events(std::vector<ClipEvent> events)
{
	if (events.size() > max_clip_events) {
		return fail(ErrorCode::InvalidArgument, "a model holds at most " + std::to_string(max_clip_events) + " clip events");
	}
	for (const ClipEvent& event : events) {
		if (!clip_name(event.clip)) {
			return fail(ErrorCode::InvalidArgument, "a clip event needs a clip name of 1 to 256 bytes without control characters");
		}
		if (!event_name(event.name)) {
			return fail(ErrorCode::InvalidArgument, "event name '" + event.name + "' must be 1 to 32 of a-z, 0-9 and _");
		}
		if (!std::isfinite(event.time) || event.time < 0 || event.time > max_clip_event_time) {
			return fail(ErrorCode::InvalidArgument, "event '" + event.name + "' needs a time within 0 to 3600 seconds");
		}
	}
	const auto key = [](const ClipEvent& e) { return std::tie(e.clip, e.time, e.name); };
	std::sort(events.begin(), events.end(), [&](const ClipEvent& a, const ClipEvent& b) { return key(a) < key(b); });
	if (const auto same = std::adjacent_find(events.begin(), events.end()); same != events.end()) {
		return fail(ErrorCode::InvalidArgument, "event '" + same->name + "' of clip '" + same->clip + "' is listed twice at " +
			shortest(same->time) + " s");
	}
	return events;
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
		if (t[0] == "event") {
			// event = <seconds> <name> "<clip>"
			bool closed = true;
			const auto e = line_tokens(line, closed);
			ClipEvent event;
			if (!closed || e.size() != 5 || e[1] != "=" || !parse_float(e[2], event.time)) {
				return parse_error(line_number, "expected 'event = <seconds> <name> \"<clip>\"'");
			}
			event.name = e[3];
			event.clip = e[4];
			meta.events.push_back(std::move(event));
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
	auto events = normalize_clip_events(std::move(meta.events));
	if (!events) {
		return fail(ErrorCode::ParseError, events.error().message);
	}
	meta.events = std::move(events.value());
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
	for (const ClipEvent& event : meta.events) {
		out += "event = " + shortest(event.time) + " " + event.name + " \"";
		for (const char c : event.clip) {
			if (c == '"' || c == '\\') {
				out += '\\';
			}
			out += c;
		}
		out += "\"\n";
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
	case AssetType::Material:
		return "material";
	case AssetType::Environment:
		return "environment";
	case AssetType::Effect:
		return "effect";
	case AssetType::Script:
		return "script";
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
	if (iequals(ext, "material")) {
		return AssetType::Material;
	}
	if (iequals(ext, "hdr")) {
		return AssetType::Environment;
	}
	if (iequals(ext, "effect")) {
		return AssetType::Effect;
	}
	if (iequals(ext, "lua")) {
		return AssetType::Script;
	}
	return AssetType::Unknown;
}

// ---- Tangents ------------------------------------------------------------------------------------

void compute_tangents(MeshData& mesh, std::uint32_t first_index, std::uint32_t index_count)
{
	using V3 = std::array<float, 3>;
	const auto sub = [](const V3& a, const V3& b) { return V3{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
	const auto dot = [](const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	const auto cross = [](const V3& a, const V3& b) {
		return V3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
	};
	std::map<std::uint32_t, std::pair<V3, V3>> sums; // Vertex: sum of +u directions, sum of +v directions.
	const std::uint32_t* indices = mesh.indices.data() + first_index;
	for (std::uint32_t i = 0; i + 2 < index_count; i += 3) {
		const MeshVertex& a = mesh.vertices[indices[i]];
		const MeshVertex& b = mesh.vertices[indices[i + 1]];
		const MeshVertex& c = mesh.vertices[indices[i + 2]];
		const V3 e1 = sub(b.position, a.position), e2 = sub(c.position, a.position);
		const float du1 = b.uv[0] - a.uv[0], dv1 = b.uv[1] - a.uv[1], du2 = c.uv[0] - a.uv[0], dv2 = c.uv[1] - a.uv[1];
		const float det = du1 * dv2 - du2 * dv1;
		if (std::abs(det) < 1.0e-12f || !std::isfinite(det)) {
			continue;
		}
		const float r = 1.0f / det;
		const V3 t{(dv2 * e1[0] - dv1 * e2[0]) * r, (dv2 * e1[1] - dv1 * e2[1]) * r, (dv2 * e1[2] - dv1 * e2[2]) * r};
		const V3 s{(du1 * e2[0] - du2 * e1[0]) * r, (du1 * e2[1] - du2 * e1[1]) * r, (du1 * e2[2] - du2 * e1[2]) * r};
		for (const std::uint32_t index : {indices[i], indices[i + 1], indices[i + 2]}) {
			auto& [tangent, bitangent] = sums[index];
			for (std::size_t k = 0; k < 3; ++k) {
				tangent[k] += t[k];
				bitangent[k] += s[k];
			}
		}
	}
	for (std::uint32_t i = 0; i < index_count; ++i) {
		MeshVertex& v = mesh.vertices[indices[i]];
		const auto found = sums.find(indices[i]);
		V3 t{};
		V3 s{};
		if (found != sums.end()) {
			t = found->second.first;
			s = found->second.second;
		}
		// Gram-Schmidt against the normal; a vertex with no usable UVs gets any frame around its normal.
		const float along = dot(t, v.normal);
		V3 ortho{t[0] - v.normal[0] * along, t[1] - v.normal[1] * along, t[2] - v.normal[2] * along};
		float length = std::sqrt(dot(ortho, ortho));
		if (!std::isfinite(length) || length < 1.0e-8f) {
			ortho = cross(std::abs(v.normal[1]) > 0.9f ? V3{0.0f, 0.0f, 1.0f} : V3{0.0f, 1.0f, 0.0f}, v.normal);
			length = std::sqrt(dot(ortho, ortho));
			s = {};
		}
		if (length < 1.0e-8f) {
			continue; // A zero normal: leave the default.
		}
		// +v runs towards the bottom of the image, and the bitangent must point at its top.
		const float handedness = dot(cross(v.normal, ortho), s) > 0.0f ? -1.0f : 1.0f;
		v.tangent = {ortho[0] / length, ortho[1] / length, ortho[2] / length, handedness};
	}
}

// ---- Material assets -----------------------------------------------------------------------------

namespace {

constexpr std::string_view material_magic = "ENGINE_MATERIAL";

const char* alpha_token(AlphaMode mode) noexcept
{
	return mode == AlphaMode::Mask ? "mask" : mode == AlphaMode::Blend ? "blend" : "opaque";
}

bool unit(double v) noexcept
{
	return std::isfinite(v) && v >= 0.0 && v <= 1.0;
}

// Whitespace-separated tokens with `"` quoted strings, of one line.
std::vector<std::string> line_tokens(std::string_view line, bool& valid)
{
	std::vector<std::string> tokens;
	std::size_t i = 0;
	while (i < line.size()) {
		if (line[i] == ' ' || line[i] == '\t' || line[i] == '\r') {
			++i;
		} else if (line[i] == '"') {
			std::string value;
			++i;
			bool closed = false;
			while (i < line.size()) {
				if (line[i] == '"') {
					closed = true;
					++i;
					break;
				}
				if (line[i] == '\\' && i + 1 < line.size()) {
					++i;
				}
				value += line[i++];
			}
			valid = valid && closed;
			tokens.push_back(std::move(value));
		} else {
			const std::size_t start = i;
			while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
				++i;
			}
			tokens.emplace_back(line.substr(start, i - start));
		}
	}
	return tokens;
}

bool number(const std::string& text, double& value) noexcept
{
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
	return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(value);
}

void write_texture(std::string& out, std::string_view key, const MaterialTexture& texture)
{
	out += key;
	out += ' ';
	if (texture.empty()) {
		out += "-\n";
		return;
	}
	out += to_string(texture.id);
	out += " \"";
	for (const char c : texture.path) {
		if (c == '"' || c == '\\') {
			out += '\\';
		}
		out += c;
	}
	out += "\"\n";
}

void write_numbers(std::string& out, std::string_view key, std::span<const double> values)
{
	out += key;
	for (const double value : values) {
		char buffer[64];
		const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
		out += ' ';
		out.append(buffer, result.ptr);
	}
	out += '\n';
}

void write_numbers(std::string& out, std::string_view key, std::initializer_list<double> values)
{
	write_numbers(out, key, std::span<const double>(values.begin(), values.size()));
}

} // namespace

Result<void> validate_material(const MaterialData& m)
{
	const auto invalid = [](const char* text) { return fail(ErrorCode::InvalidArgument, std::string("Invalid material: ") + text); };
	for (const double c : m.base_color) {
		if (!unit(c)) {
			return invalid("base color must be within [0, 1]");
		}
	}
	for (const double c : m.emissive) {
		if (!unit(c)) {
			return invalid("emissive color must be within [0, 1]");
		}
	}
	if (!unit(m.metallic) || !unit(m.roughness) || !unit(m.occlusion_strength) || !unit(m.alpha_cutoff)) {
		return invalid("metallic, roughness, occlusion strength and alpha cutoff must be within [0, 1]");
	}
	if (!std::isfinite(m.emissive_strength) || m.emissive_strength < 0.0 || !std::isfinite(m.normal_scale) || m.normal_scale < 0.0) {
		return invalid("emissive strength and normal scale must be >= 0");
	}
	if (!std::isfinite(m.uv_scale[0]) || !std::isfinite(m.uv_scale[1]) || m.uv_scale[0] <= 0.0 || m.uv_scale[1] <= 0.0) {
		return invalid("UV scale must be positive");
	}
	if (m.alpha_mode != AlphaMode::Opaque && m.alpha_mode != AlphaMode::Mask && m.alpha_mode != AlphaMode::Blend) {
		return invalid("unknown alpha mode");
	}
	for (const MaterialTexture* texture : {&m.base_color_map, &m.metallic_roughness_map, &m.normal_map, &m.occlusion_map, &m.emissive_map}) {
		if (texture->empty() != texture->path.empty() || texture->path.size() > max_path_bytes) {
			return invalid("a texture reference needs both an ID and a short path, or neither");
		}
	}
	return {};
}

std::string write_material(const MaterialData& m)
{
	std::string out(material_magic);
	out += " 1\n";
	write_numbers(out, "base_color", {m.base_color[0], m.base_color[1], m.base_color[2], m.base_color[3]});
	write_numbers(out, "metallic", {m.metallic});
	write_numbers(out, "roughness", {m.roughness});
	write_numbers(out, "emissive", {m.emissive[0], m.emissive[1], m.emissive[2]});
	write_numbers(out, "emissive_strength", {m.emissive_strength});
	write_numbers(out, "normal_scale", {m.normal_scale});
	write_numbers(out, "occlusion_strength", {m.occlusion_strength});
	out += "alpha_mode ";
	out += alpha_token(m.alpha_mode);
	out += '\n';
	write_numbers(out, "alpha_cutoff", {m.alpha_cutoff});
	write_numbers(out, "double_sided", {m.double_sided ? 1.0 : 0.0});
	write_numbers(out, "uv_scale", {m.uv_scale[0], m.uv_scale[1]});
	write_texture(out, "base_color_map", m.base_color_map);
	write_texture(out, "metallic_roughness_map", m.metallic_roughness_map);
	write_texture(out, "normal_map", m.normal_map);
	write_texture(out, "occlusion_map", m.occlusion_map);
	write_texture(out, "emissive_map", m.emissive_map);
	return out;
}

Result<MaterialData> parse_material(std::string_view text)
{
	if (text.size() > 64 * 1024) {
		return fail(ErrorCode::InvalidArgument, "the material file is too large");
	}
	const auto malformed = [](const std::string& why) { return fail(ErrorCode::ParseError, "Invalid material: " + why); };
	MaterialData m;
	std::vector<std::string> seen;
	bool header = false;
	std::size_t start = 0;
	while (start <= text.size()) {
		const std::size_t end = std::min(text.find('\n', start), text.size());
		const std::string_view line = text.substr(start, end - start);
		start = end + 1;
		bool valid = true;
		const std::vector<std::string> tokens = line_tokens(line, valid);
		if (tokens.empty()) {
			continue;
		}
		if (!valid) {
			return malformed("unterminated string");
		}
		if (!header) {
			if (tokens.size() != 2 || tokens[0] != material_magic || tokens[1] != "1") {
				return malformed("missing header or unsupported version");
			}
			header = true;
			continue;
		}
		const std::string& key = tokens[0];
		if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
			return malformed("duplicate key " + key);
		}
		seen.push_back(key);
		const auto numbers = [&](std::initializer_list<double*> targets) {
			if (tokens.size() != targets.size() + 1) {
				return false;
			}
			std::size_t i = 1;
			for (double* target : targets) {
				if (!number(tokens[i++], *target)) {
					return false;
				}
			}
			return true;
		};
		const auto texture = [&](MaterialTexture& out) {
			if (tokens.size() == 2 && tokens[1] == "-") {
				out = {};
				return true;
			}
			if (tokens.size() != 3) {
				return false;
			}
			const auto id = parse_asset_id(tokens[1]);
			if (!id) {
				return false;
			}
			out = {*id, tokens[2]};
			return true;
		};
		bool parsed = false;
		if (key == "base_color") {
			parsed = numbers({&m.base_color[0], &m.base_color[1], &m.base_color[2], &m.base_color[3]});
		} else if (key == "metallic") {
			parsed = numbers({&m.metallic});
		} else if (key == "roughness") {
			parsed = numbers({&m.roughness});
		} else if (key == "emissive") {
			parsed = numbers({&m.emissive[0], &m.emissive[1], &m.emissive[2]});
		} else if (key == "emissive_strength") {
			parsed = numbers({&m.emissive_strength});
		} else if (key == "normal_scale") {
			parsed = numbers({&m.normal_scale});
		} else if (key == "occlusion_strength") {
			parsed = numbers({&m.occlusion_strength});
		} else if (key == "alpha_mode") {
			parsed = tokens.size() == 2 && (tokens[1] == "opaque" || tokens[1] == "mask" || tokens[1] == "blend");
			if (parsed) {
				m.alpha_mode = tokens[1] == "mask" ? AlphaMode::Mask : tokens[1] == "blend" ? AlphaMode::Blend : AlphaMode::Opaque;
			}
		} else if (key == "alpha_cutoff") {
			parsed = numbers({&m.alpha_cutoff});
		} else if (key == "double_sided") {
			double flag = 0.0;
			parsed = numbers({&flag}) && (flag == 0.0 || flag == 1.0);
			m.double_sided = flag == 1.0;
		} else if (key == "uv_scale") {
			parsed = numbers({&m.uv_scale[0], &m.uv_scale[1]});
		} else if (key == "base_color_map") {
			parsed = texture(m.base_color_map);
		} else if (key == "metallic_roughness_map") {
			parsed = texture(m.metallic_roughness_map);
		} else if (key == "normal_map") {
			parsed = texture(m.normal_map);
		} else if (key == "occlusion_map") {
			parsed = texture(m.occlusion_map);
		} else if (key == "emissive_map") {
			parsed = texture(m.emissive_map);
		} else {
			return malformed("unknown key " + key);
		}
		if (!parsed) {
			return malformed("bad value for " + key);
		}
	}
	if (!header) {
		return malformed("missing header or unsupported version");
	}
	if (auto valid = validate_material(m); !valid) {
		return valid.error();
	}
	return m;
}

// ---- Effect assets -------------------------------------------------------------------------------

namespace {

constexpr std::string_view effect_magic = "ENGINE_EFFECT";

} // namespace

Result<void> validate_effect(const EffectData& e)
{
	const auto invalid = [](const char* text) { return fail(ErrorCode::InvalidArgument, std::string("Invalid effect: ") + text); };
	const auto finite = [](double v) { return std::isfinite(v); };
	const auto nonnegative = [](double v) { return std::isfinite(v) && v >= 0.0; };
	if (!finite(e.duration) || e.duration <= 0.0 || e.duration > 3600.0) {
		return invalid("duration must be within (0, 3600] seconds");
	}
	if (!nonnegative(e.rate) || e.rate > 100000.0 || e.burst > max_effect_particles) {
		return invalid("rate must be within [0, 100000] and burst at most 20000");
	}
	if (!finite(e.lifetime[0]) || !finite(e.lifetime[1]) || e.lifetime[0] <= 0.0 || e.lifetime[1] < e.lifetime[0] || e.lifetime[1] > 600.0) {
		return invalid("lifetime needs 0 < min <= max <= 600 seconds");
	}
	if (!nonnegative(e.speed[0]) || !finite(e.speed[1]) || e.speed[1] < e.speed[0] || e.speed[1] > 10000.0) {
		return invalid("speed needs 0 <= min <= max");
	}
	if (!finite(e.cone_degrees) || e.cone_degrees < 0.0 || e.cone_degrees > 180.0) {
		return invalid("cone must be within [0, 180] degrees");
	}
	if (!nonnegative(e.spawn_radius) || !nonnegative(e.drag) || !nonnegative(e.stretch) || !nonnegative(e.emissive_strength)) {
		return invalid("spawn radius, drag, stretch and emissive strength must be >= 0");
	}
	if (!finite(e.acceleration[0]) || !finite(e.acceleration[1]) || !finite(e.acceleration[2]) || !finite(e.spin_degrees[0]) ||
		!finite(e.spin_degrees[1]) || e.spin_degrees[1] < e.spin_degrees[0] || !finite(e.floor_height) || !unit(e.bounce)) {
		return invalid("acceleration, spin and floor must be finite, spin min <= max, bounce within [0, 1]");
	}
	if (e.blend != ParticleBlend::Additive && e.blend != ParticleBlend::Alpha) {
		return invalid("unknown blend mode");
	}
	if (e.max_particles < 1 || e.max_particles > max_effect_particles) {
		return invalid("max particles must be within [1, 20000]");
	}
	const auto keys_valid = [](const auto& keys, auto&& value_valid) {
		if (keys.empty() || keys.size() > max_effect_keys) {
			return false;
		}
		double previous = -1.0;
		for (const auto& key : keys) {
			if (!unit(key.time) || key.time <= previous || !value_valid(key)) {
				return false;
			}
			previous = key.time;
		}
		return true;
	};
	if (!keys_valid(e.size, [&](const ParticleSizeKey& k) { return nonnegative(k.size) && k.size <= 1000.0; })) {
		return invalid("size needs 1 to 8 keys at ascending times within [0, 1], sizes within [0, 1000]");
	}
	if (!keys_valid(e.color, [](const ParticleColorKey& k) {
			return unit(k.color[0]) && unit(k.color[1]) && unit(k.color[2]) && unit(k.color[3]);
		})) {
		return invalid("color needs 1 to 8 keys at ascending times within [0, 1], channels within [0, 1]");
	}
	if (e.texture.empty() != e.texture.path.empty() || e.texture.path.size() > max_path_bytes) {
		return invalid("a texture reference needs both an ID and a short path, or neither");
	}
	return {};
}

std::string write_effect(const EffectData& e)
{
	std::string out(effect_magic);
	out += " 1\n";
	write_numbers(out, "duration", {e.duration});
	write_numbers(out, "rate", {e.rate});
	write_numbers(out, "burst", {static_cast<double>(e.burst)});
	write_numbers(out, "lifetime", {e.lifetime[0], e.lifetime[1]});
	write_numbers(out, "speed", {e.speed[0], e.speed[1]});
	write_numbers(out, "cone", {e.cone_degrees});
	write_numbers(out, "spawn_radius", {e.spawn_radius});
	write_numbers(out, "acceleration", {e.acceleration[0], e.acceleration[1], e.acceleration[2]});
	write_numbers(out, "drag", {e.drag});
	write_numbers(out, "spin", {e.spin_degrees[0], e.spin_degrees[1]});
	write_numbers(out, "stretch", {e.stretch});
	std::vector<double> values;
	for (const auto& key : e.size) {
		values.insert(values.end(), {key.time, key.size});
	}
	write_numbers(out, "size", values);
	values.clear();
	for (const auto& key : e.color) {
		values.insert(values.end(), {key.time, key.color[0], key.color[1], key.color[2], key.color[3]});
	}
	write_numbers(out, "color", values);
	write_numbers(out, "emissive_strength", {e.emissive_strength});
	out += e.blend == ParticleBlend::Alpha ? "blend alpha\n" : "blend additive\n";
	write_texture(out, "texture", e.texture);
	write_numbers(out, "collide_floor", {e.collide_floor ? 1.0 : 0.0});
	write_numbers(out, "floor_height", {e.floor_height});
	write_numbers(out, "bounce", {e.bounce});
	write_numbers(out, "world_space", {e.world_space ? 1.0 : 0.0});
	write_numbers(out, "max_particles", {static_cast<double>(e.max_particles)});
	return out;
}

Result<EffectData> parse_effect(std::string_view text)
{
	if (text.size() > 64 * 1024) {
		return fail(ErrorCode::InvalidArgument, "the effect file is too large");
	}
	const auto malformed = [](const std::string& why) { return fail(ErrorCode::ParseError, "Invalid effect: " + why); };
	EffectData e;
	std::vector<std::string> seen;
	bool header = false;
	std::size_t start = 0;
	while (start <= text.size()) {
		const std::size_t end = std::min(text.find('\n', start), text.size());
		const std::string_view line = text.substr(start, end - start);
		start = end + 1;
		bool valid = true;
		const std::vector<std::string> tokens = line_tokens(line, valid);
		if (tokens.empty()) {
			continue;
		}
		if (!valid) {
			return malformed("unterminated string");
		}
		if (!header) {
			if (tokens.size() != 2 || tokens[0] != effect_magic || tokens[1] != "1") {
				return malformed("missing header or unsupported version");
			}
			header = true;
			continue;
		}
		const std::string& key = tokens[0];
		if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
			return malformed("duplicate key " + key);
		}
		seen.push_back(key);
		std::vector<double> v;
		const auto read_numbers = [&] {
			for (std::size_t i = 1; i < tokens.size(); ++i) {
				double value = 0.0;
				if (!number(tokens[i], value)) {
					return false;
				}
				v.push_back(value);
			}
			return true;
		};
		const auto count = [&](std::size_t n) { return read_numbers() && v.size() == n; };
		const auto flag = [&](bool& out) {
			if (!count(1) || (v[0] != 0.0 && v[0] != 1.0)) {
				return false;
			}
			out = v[0] == 1.0;
			return true;
		};
		const auto whole = [&](std::uint32_t& out) {
			if (!count(1) || v[0] < 0.0 || v[0] > 4294967295.0 || v[0] != std::floor(v[0])) {
				return false;
			}
			out = static_cast<std::uint32_t>(v[0]);
			return true;
		};
		bool parsed = false;
		if (key == "duration") {
			parsed = count(1) && ((e.duration = v[0]), true);
		} else if (key == "rate") {
			parsed = count(1) && ((e.rate = v[0]), true);
		} else if (key == "burst") {
			parsed = whole(e.burst);
		} else if (key == "lifetime") {
			parsed = count(2) && ((e.lifetime = {v[0], v[1]}), true);
		} else if (key == "speed") {
			parsed = count(2) && ((e.speed = {v[0], v[1]}), true);
		} else if (key == "cone") {
			parsed = count(1) && ((e.cone_degrees = v[0]), true);
		} else if (key == "spawn_radius") {
			parsed = count(1) && ((e.spawn_radius = v[0]), true);
		} else if (key == "acceleration") {
			parsed = count(3) && ((e.acceleration = {v[0], v[1], v[2]}), true);
		} else if (key == "drag") {
			parsed = count(1) && ((e.drag = v[0]), true);
		} else if (key == "spin") {
			parsed = count(2) && ((e.spin_degrees = {v[0], v[1]}), true);
		} else if (key == "stretch") {
			parsed = count(1) && ((e.stretch = v[0]), true);
		} else if (key == "size") {
			parsed = read_numbers() && !v.empty() && v.size() % 2 == 0;
			e.size.clear();
			for (std::size_t i = 0; parsed && i < v.size(); i += 2) {
				e.size.push_back({v[i], v[i + 1]});
			}
		} else if (key == "color") {
			parsed = read_numbers() && !v.empty() && v.size() % 5 == 0;
			e.color.clear();
			for (std::size_t i = 0; parsed && i < v.size(); i += 5) {
				e.color.push_back({v[i], {v[i + 1], v[i + 2], v[i + 3], v[i + 4]}});
			}
		} else if (key == "emissive_strength") {
			parsed = count(1) && ((e.emissive_strength = v[0]), true);
		} else if (key == "blend") {
			parsed = tokens.size() == 2 && (tokens[1] == "additive" || tokens[1] == "alpha");
			e.blend = parsed && tokens[1] == "alpha" ? ParticleBlend::Alpha : ParticleBlend::Additive;
		} else if (key == "texture") {
			if (tokens.size() == 2 && tokens[1] == "-") {
				e.texture = {};
				parsed = true;
			} else if (tokens.size() == 3) {
				if (const auto id = parse_asset_id(tokens[1])) {
					e.texture = {*id, tokens[2]};
					parsed = true;
				}
			}
		} else if (key == "collide_floor") {
			parsed = flag(e.collide_floor);
		} else if (key == "floor_height") {
			parsed = count(1) && ((e.floor_height = v[0]), true);
		} else if (key == "bounce") {
			parsed = count(1) && ((e.bounce = v[0]), true);
		} else if (key == "world_space") {
			parsed = flag(e.world_space);
		} else if (key == "max_particles") {
			parsed = whole(e.max_particles);
		} else {
			return malformed("unknown key " + key);
		}
		if (!parsed) {
			return malformed("bad value for " + key);
		}
	}
	if (!header) {
		return malformed("missing header or unsupported version");
	}
	if (auto valid = validate_effect(e); !valid) {
		return valid.error();
	}
	return e;
}

} // namespace engine::assets
