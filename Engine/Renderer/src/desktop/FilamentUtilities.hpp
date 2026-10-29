#pragma once
#include <filament/Engine.h>
#include <filament/Material.h>
#include <backend/BufferDescriptor.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
namespace engine::renderer::detail {
inline void *copy_bytes(const void *data, std::size_t size) {
    void *copy = std::malloc(size);
    if (!copy) throw std::bad_alloc();
    std::memcpy(copy, data, size); return copy;
}
inline void release(void *buffer, std::size_t, void *) { std::free(buffer); }
inline filament::backend::BufferDescriptor upload(const void *data, std::size_t size) {
    return {copy_bytes(data, size), size, release};
}
inline filament::Material *material(filament::Engine &engine, const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("Missing compiled material: " + file.string());
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in), {}};
    auto *result = filament::Material::Builder().package(bytes.data(), bytes.size()).build(engine);
    if (!result) throw std::runtime_error("Invalid material: " + file.string());
    return result;
}
}
