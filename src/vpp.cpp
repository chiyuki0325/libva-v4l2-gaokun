// SPDX-License-Identifier: MIT
#include "internal.hpp"

namespace irisva {
bool vpp_supported(VAProfile, VAEntrypoint) {
    return false;
}
void vpp_caps(VAProcPipelineCaps &caps) {
    // These are driver-owned arrays, as consumed by FFmpeg and other VA clients.
    // Do not read the incoming structure: clients may pass it uninitialized.
    static VAProcColorStandardType colors[] = {VAProcColorStandardNone, VAProcColorStandardBT601,
                                               VAProcColorStandardBT709, VAProcColorStandardBT2020,
                                               VAProcColorStandardExplicit};
    static uint32_t formats[] = {VA_FOURCC_NV12};
    caps = {};
    caps.input_color_standards = caps.output_color_standards = colors;
    caps.num_input_color_standards = caps.num_output_color_standards =
        sizeof(colors) / sizeof(colors[0]);
    caps.input_pixel_format = caps.output_pixel_format = formats;
    caps.num_input_pixel_formats = caps.num_output_pixel_formats = 1;
    caps.min_input_width = caps.min_output_width = 16;
    caps.min_input_height = caps.min_output_height = 2;
    caps.max_input_width = caps.max_output_width = 8192;
    caps.max_input_height = caps.max_output_height = 8192;
}
VppPicture vpp_picture(const Buffer &buffer, const std::shared_ptr<Surface> &source,
                       const Surface &target) {
    VAProcPipelineParameterBuffer p{};
    std::memcpy(&p, buffer.data.data(), sizeof(p));
    check(!p.num_filters && !p.num_forward_references && !p.num_backward_references &&
              !p.rotation_state && !p.mirror_state && !p.blend_state && !p.num_additional_outputs &&
              !(p.pipeline_flags & ~VA_PROC_PIPELINE_FAST) &&
              !(p.filter_flags & ~VA_FILTER_SCALING_MASK) && !p.input_surface_flag &&
              !p.output_surface_flag && !p.output_hdr_metadata,
          "VPP supports progressive scaling only", VA_STATUS_ERROR_UNIMPLEMENTED);
    const auto &in = p.input_color_properties;
    const auto &out = p.output_color_properties;
    auto matches = [](unsigned a, unsigned b) { return !a || !b || a == b; };
    check(matches(p.surface_color_standard, p.output_color_standard) &&
              matches(in.color_range, out.color_range) &&
              matches(in.chroma_sample_location, out.chroma_sample_location) &&
              matches(in.colour_primaries, out.colour_primaries) &&
              matches(in.transfer_characteristics, out.transfer_characteristics) &&
              matches(in.matrix_coefficients, out.matrix_coefficients),
          "VPP color conversion unsupported", VA_STATUS_ERROR_UNIMPLEMENTED);
    check(!p.output_region ||
              (!p.output_region->x && !p.output_region->y &&
               p.output_region->width == target.width && p.output_region->height == target.height),
          "VPP output must cover the entire surface", VA_STATUS_ERROR_UNIMPLEMENTED);
    VppPicture picture;
    picture.source = source;
    picture.region = p.surface_region ? *p.surface_region
                                      : VARectangle{0, 0, static_cast<uint16_t>(source->width),
                                                    static_cast<uint16_t>(source->height)};
    auto r = picture.region;
    check(r.x >= 0 && r.y >= 0 && r.width && r.height && !(r.x & 15) && !(r.y & 1) &&
              !(r.width & 15) && !(r.height & 1) && !(target.width & 15) && !(target.height & 1) &&
              unsigned(r.x) + r.width <= source->width &&
              unsigned(r.y) + r.height <= source->height,
          "VPP requires 16-pixel widths/aligned crop x and even heights/crop y",
          VA_STATUS_ERROR_INVALID_PARAMETER);
    bool down = target.width <= r.width && target.height <= r.height;
    bool up = target.width >= r.width && target.height >= r.height;
    check((down || up) && uint64_t(target.width) * 20 >= r.width &&
              uint64_t(target.height) * 20 >= r.height,
          "VPP cannot mix up/down scaling axes or downscale beyond 20:1",
          VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    check(source.get() != &target && source->fourcc == VA_FOURCC_NV12 &&
              target.fourcc == VA_FOURCC_NV12,
          "VPP requires distinct NV12 surfaces", VA_STATUS_ERROR_INVALID_SURFACE);
    return picture;
}
Vpp::Vpp() {
    throw Error(VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT, "VPP is unavailable");
}
void Vpp::scale(const VppPicture &, const std::shared_ptr<Surface> &) {
    throw Error(VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT, "VPP is unavailable");
}
} // namespace irisva
