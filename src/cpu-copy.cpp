// SPDX-License-Identifier: MIT
#include "internal.hpp"

namespace irisva {
struct SurfaceCopier::Impl {
    Impl() {
        const char *mode = std::getenv("IRIS_VAAPI_COPY");
        check(!mode || !strcmp(mode, "gpu") || !strcmp(mode, "cpu"),
              "IRIS_VAAPI_COPY must be gpu or cpu", VA_STATUS_ERROR_INVALID_PARAMETER);
        check(!mode || strcmp(mode, "gpu"),
              "GPU copy support was disabled at build time; use IRIS_VAAPI_COPY=cpu",
              VA_STATUS_ERROR_UNIMPLEMENTED);
        trace("surface copy selected mode=%s", "cpu");
    }
};

SurfaceCopier::SurfaceCopier(int render_fd) : render_fd_(render_fd) {}
SurfaceCopier::~SurfaceCopier() = default;
void SurfaceCopier::copy(const std::shared_ptr<Memory> &dst, const std::shared_ptr<Memory> &src,
                         unsigned w, unsigned h) {
    if (!impl_)
        impl_ = std::make_unique<Impl>();
    copy_surface(*dst, *src, w, h);
}
} // namespace irisva
