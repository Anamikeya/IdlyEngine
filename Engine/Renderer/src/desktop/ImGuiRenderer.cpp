#include "ImGuiRenderer.hpp"
#include "FilamentUtilities.hpp"
#include <imgui.h>
#include <filament/Camera.h>
#include <filament/IndexBuffer.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/VertexBuffer.h>
#include <filament/Viewport.h>
#include <utils/EntityManager.h>
#include <algorithm>

namespace engine::renderer::detail {
using namespace filament;
ImGuiRenderer::ImGuiRenderer(Engine &e, const std::filesystem::path &path) : engine_(e) {
    material_ = material(e, path / "ui.filamat");
    view_ = e.createView(); scene_ = e.createScene();
    camera_entity_ = utils::EntityManager::get().create();
    renderable_ = utils::EntityManager::get().create();
    camera_ = e.createCamera(camera_entity_);
    view_->setCamera(camera_); view_->setScene(scene_);
    view_->setPostProcessingEnabled(false);
    view_->setShadowingEnabled(false);
    view_->setBlendMode(View::BlendMode::TRANSLUCENT);
    scene_->addEntity(renderable_);
    auto &io = ImGui::GetIO();
    io.BackendRendererName = "engine_filament";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    unsigned char *pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    font_ = Texture::Builder().width(width).height(height).levels(1)
        .format(Texture::InternalFormat::RGBA8).sampler(Texture::Sampler::SAMPLER_2D).build(e);
    const std::size_t size = static_cast<std::size_t>(width) * height * 4;
    font_->setImage(e, 0, Texture::PixelBufferDescriptor(copy_bytes(pixels, size), size,
        Texture::Format::RGBA, Texture::Type::UBYTE, release));
    io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(font_));
}
ImGuiRenderer::~ImGuiRenderer() {
    engine_.destroy(view_); engine_.destroy(scene_); engine_.destroy(renderable_);
    engine_.destroyCameraComponent(camera_entity_);
    for (auto *m : materials_) engine_.destroy(m);
    engine_.destroy(vertices_); engine_.destroy(indices_);
    engine_.destroy(material_); engine_.destroy(font_);
    utils::EntityManager::get().destroy(renderable_);
    utils::EntityManager::get().destroy(camera_entity_);
    ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
    ImGui::GetIO().BackendRendererName = nullptr;
}
void ImGuiRenderer::update(ImDrawData *data, unsigned width, unsigned height) {
    view_->setViewport({0, 0, width, height});
    engine_.getRenderableManager().destroy(renderable_);
    if (!data || data->TotalVtxCount == 0 || !width || !height) return;
    const auto p = data->DisplayPos;
    camera_->setProjection(Camera::Projection::ORTHO, p.x, p.x + data->DisplaySize.x,
        p.y + data->DisplaySize.y, p.y, 0, 1);
    struct Draw { const ImDrawCmd *cmd; unsigned offset; };
    std::vector<ImDrawVert> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Draw> draws;
    vertices.reserve(data->TotalVtxCount); indices.reserve(data->TotalIdxCount);
    for (const auto *list : data->CmdLists) {
        const auto base = static_cast<std::uint32_t>(vertices.size());
        vertices.insert(vertices.end(), list->VtxBuffer.begin(), list->VtxBuffer.end());
        for (const auto &cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                continue;
            }
            if (!cmd.ElemCount) continue;
            draws.push_back({&cmd, static_cast<unsigned>(indices.size())});
            for (unsigned i = 0; i < cmd.ElemCount; ++i)
                indices.push_back(base + cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i]);
        }
    }
    if (draws.empty()) return;
    if (!vertices_ || vertices_->getVertexCount() < vertices.size() || !indices_ || indices_->getIndexCount() < indices.size()) {
        engine_.flushAndWait();
        engine_.destroy(vertices_); engine_.destroy(indices_);
        vertices_ = VertexBuffer::Builder().vertexCount(static_cast<uint32_t>(vertices.size()) + 4096).bufferCount(1)
            .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT2, offsetof(ImDrawVert, pos), sizeof(ImDrawVert))
            .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2, offsetof(ImDrawVert, uv), sizeof(ImDrawVert))
            .attribute(VertexAttribute::COLOR, 0, VertexBuffer::AttributeType::UBYTE4, offsetof(ImDrawVert, col), sizeof(ImDrawVert))
            .normalized(VertexAttribute::COLOR).build(engine_);
        indices_ = IndexBuffer::Builder().indexCount(static_cast<uint32_t>(indices.size()) + 8192)
            .bufferType(IndexBuffer::IndexType::UINT).build(engine_);
    }
    vertices_->setBufferAt(engine_, 0, upload(vertices.data(), vertices.size() * sizeof(ImDrawVert)));
    indices_->setBuffer(engine_, upload(indices.data(), indices.size() * sizeof(std::uint32_t)));
    while (materials_.size() < draws.size()) materials_.push_back(material_->createInstance());
    RenderableManager::Builder builder(draws.size());
    builder.boundingBox({{0, 0, 0}, {10000, 10000, 10000}}).culling(false).castShadows(false).receiveShadows(false);
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const auto &cmd = *draws[i].cmd;
        const auto scale = data->FramebufferScale;
        const float left = std::clamp((cmd.ClipRect.x - p.x) * scale.x, 0.f, float(width));
        const float top = std::clamp((cmd.ClipRect.y - p.y) * scale.y, 0.f, float(height));
        const float right = std::clamp((cmd.ClipRect.z - p.x) * scale.x, left, float(width));
        const float bottom = std::clamp((cmd.ClipRect.w - p.y) * scale.y, top, float(height));
        auto *m = materials_[i];
        m->setScissor(unsigned(left), unsigned(float(height) - bottom), unsigned(right - left), unsigned(bottom - top));
        m->setParameter("albedo", reinterpret_cast<Texture *>(cmd.GetTexID()),
            TextureSampler(TextureSampler::MinFilter::LINEAR, TextureSampler::MagFilter::LINEAR));
        builder.geometry(i, RenderableManager::PrimitiveType::TRIANGLES, vertices_, indices_, draws[i].offset, cmd.ElemCount)
            .material(i, m).blendOrder(i, static_cast<uint16_t>(i));
    }
    builder.build(engine_, renderable_);
}
}
