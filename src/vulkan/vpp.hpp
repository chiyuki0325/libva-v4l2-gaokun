// SPDX-License-Identifier: MIT
#pragma once
#include <memory>

namespace irisva {
struct Surface;
struct VppPicture;
class VulkanVpp {
    struct Impl;
    std::shared_ptr<Impl> impl_;

  public:
    explicit VulkanVpp(int render_fd);
    void process(const VppPicture &picture, Surface &target);
};
} // namespace irisva
