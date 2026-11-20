// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include "video_core/renderer_base.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace Null {

class RendererNull final : public VideoCore::RendererBase {
public:
    /// frames_displayed: Composite tells the window a frame was displayed (native Metal NVN
    /// reports its own presents instead).
    explicit RendererNull(Core::Frontend::EmuWindow& emu_window, Tegra::GPU& gpu,
                          std::unique_ptr<Core::Frontend::GraphicsContext> context,
                          bool frames_displayed = true);
    ~RendererNull() override;

    void Composite(std::span<const Tegra::FramebufferConfig> framebuffer) override;

    std::vector<u8> GetAppletCaptureBuffer() override;

    VideoCore::RasterizerInterface* ReadRasterizer() override {
        return &m_rasterizer;
    }

    [[nodiscard]] std::string GetDeviceVendor() const override {
        return "NULL";
    }

private:
    Tegra::GPU& m_gpu;
    RasterizerNull m_rasterizer;
    bool m_frames_displayed;
};

} // namespace Null
