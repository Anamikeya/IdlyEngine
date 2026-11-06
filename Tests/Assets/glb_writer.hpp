#pragma once

// Builds binary glTF files in memory for tests: views and accessors over one binary chunk, and a document around them.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace engine::test {

// Lays out a binary chunk and its views and accessors, then wraps a document around them.
struct Glb {
	std::vector<std::uint8_t> bin;
	std::vector<std::string> views, accessors;

	static std::string number(double v)
	{
		char text[40];
		std::snprintf(text, sizeof text, "%.9g", v);
		return text;
	}
	static const char* type_of(std::size_t width)
	{
		return width == 1 ? "SCALAR" : width == 2 ? "VEC2" : width == 3 ? "VEC3" : width == 4 ? "VEC4" : "MAT4";
	}
	int view(const void* data, std::size_t bytes)
	{
		while (bin.size() % 4 != 0) {
			bin.push_back(0);
		}
		views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(bin.size()) + R"(,"byteLength":)" + std::to_string(bytes) + "}");
		const auto* p = static_cast<const std::uint8_t*>(data);
		bin.insert(bin.end(), p, p + bytes);
		return static_cast<int>(views.size()) - 1;
	}
	// `bounds` writes min and max, which POSITION and animation times must have.
	int floats(const std::vector<float>& values, std::size_t width, bool bounds = false)
	{
		const int v = view(values.data(), values.size() * sizeof(float));
		std::string text = R"({"bufferView":)" + std::to_string(v) + R"(,"componentType":5126,"count":)" +
			std::to_string(values.size() / width) + R"(,"type":")" + type_of(width) + "\"";
		if (bounds) {
			std::vector<float> low(width, std::numeric_limits<float>::max()), high(width, std::numeric_limits<float>::lowest());
			for (std::size_t i = 0; i < values.size(); ++i) {
				low[i % width] = std::min(low[i % width], values[i]);
				high[i % width] = std::max(high[i % width], values[i]);
			}
			const auto list = [](const std::vector<float>& l) {
				std::string out = "[";
				for (std::size_t i = 0; i < l.size(); ++i) {
					out += (i ? "," : "") + number(l[i]);
				}
				return out + "]";
			};
			text += R"(,"min":)" + list(low) + R"(,"max":)" + list(high);
		}
		accessors.push_back(text + "}");
		return static_cast<int>(accessors.size()) - 1;
	}
	int u16(const std::vector<std::uint16_t>& values, std::size_t width)
	{
		const int v = view(values.data(), values.size() * sizeof(std::uint16_t));
		accessors.push_back(R"({"bufferView":)" + std::to_string(v) + R"(,"componentType":5123,"count":)" +
			std::to_string(values.size() / width) + R"(,"type":")" + type_of(width) + "\"}");
		return static_cast<int>(accessors.size()) - 1;
	}
	// `body` is the document without asset, buffers, bufferViews and accessors.
	std::vector<std::uint8_t> build(const std::string& body) const
	{
		std::string json = R"({"asset":{"version":"2.0"},)" + body + R"(,"buffers":[{"byteLength":)" + std::to_string(bin.size()) +
			R"(}],"bufferViews":[)";
		for (std::size_t i = 0; i < views.size(); ++i) {
			json += (i ? "," : "") + views[i];
		}
		json += R"(],"accessors":[)";
		for (std::size_t i = 0; i < accessors.size(); ++i) {
			json += (i ? "," : "") + accessors[i];
		}
		json += "]}";
		while (json.size() % 4 != 0) {
			json.push_back(' ');
		}
		std::vector<std::uint8_t> chunk = bin;
		while (chunk.size() % 4 != 0) {
			chunk.push_back(0);
		}
		std::vector<std::uint8_t> glb;
		const auto u32 = [&](std::uint32_t v) {
			for (int i = 0; i < 4; ++i) {
				glb.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
			}
		};
		glb.insert(glb.end(), {'g', 'l', 'T', 'F'});
		u32(2);
		u32(static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + chunk.size()));
		u32(static_cast<std::uint32_t>(json.size()));
		u32(0x4E4F534A);
		glb.insert(glb.end(), json.begin(), json.end());
		u32(static_cast<std::uint32_t>(chunk.size()));
		u32(0x004E4942);
		glb.insert(glb.end(), chunk.begin(), chunk.end());
		return glb;
	}
};

} // namespace engine::test
