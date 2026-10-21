// SPDX-License-Identifier: MIT
// Test-only shim: decode into pre-exported or PRIME-imported storage.
// c++ -shared -fPIC surface-storage.cpp -ldl -pthread -o surface-storage.so
// Set IRIS_TEST_STORAGE=export|prime1|prime2 alongside error-completion.so.
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace {
struct Identity {
    dev_t device;
    ino_t inode;
};
std::mutex mutex;
std::map<VASurfaceID, Identity> storage;
auto create_surfaces =
    reinterpret_cast<decltype(&vaCreateSurfaces)>(dlsym(RTLD_NEXT, "vaCreateSurfaces"));
auto destroy_surfaces =
    reinterpret_cast<decltype(&vaDestroySurfaces)>(dlsym(RTLD_NEXT, "vaDestroySurfaces"));
auto export_surface =
    reinterpret_cast<decltype(&vaExportSurfaceHandle)>(dlsym(RTLD_NEXT, "vaExportSurfaceHandle"));
VAStatus describe(VADisplay display, VASurfaceID surface, VADRMPRIMESurfaceDescriptor &desc) {
    return export_surface(display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                          VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &desc);
}
void close_objects(const VADRMPRIMESurfaceDescriptor &desc) {
    for (unsigned i = 0; i < desc.num_objects; ++i)
        close(desc.objects[i].fd);
}
} // namespace

VAStatus vaCreateSurfaces(VADisplay display, unsigned format, unsigned width, unsigned height,
                          VASurfaceID *surfaces, unsigned count, VASurfaceAttrib *attrs,
                          unsigned attr_count) {
    VAStatus result =
        create_surfaces(display, format, width, height, surfaces, count, attrs, attr_count);
    const char *mode = std::getenv("IRIS_TEST_STORAGE");
    if (result != VA_STATUS_SUCCESS || !mode)
        return result;
    for (unsigned i = 0; i < count; ++i) {
        VADRMPRIMESurfaceDescriptor desc{};
        result = describe(display, surfaces[i], desc);
        if (result != VA_STATUS_SUCCESS)
            return result;
        if (desc.num_objects != 1 || desc.num_layers != 1) {
            close_objects(desc);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        struct stat info{};
        if (fstat(desc.objects[0].fd, &info)) {
            close_objects(desc);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (std::strcmp(mode, "export")) {
            // Use the allocation's padding to exercise a nonzero plane base.
            unsigned end = desc.layers[0].offset[1] + desc.layers[0].pitch[1] * ((height + 1) / 2);
            if (desc.objects[0].size >= end + 128) {
                for (unsigned p = 0; p < desc.layers[0].num_planes; ++p)
                    desc.layers[0].offset[p] += 128;
            }
            VASurfaceAttribExternalBuffers legacy{};
            uintptr_t fd = desc.objects[0].fd;
            legacy.pixel_format = desc.fourcc;
            legacy.width = width;
            legacy.height = height;
            legacy.data_size = desc.objects[0].size;
            legacy.num_planes = desc.layers[0].num_planes;
            for (unsigned p = 0; p < legacy.num_planes; ++p) {
                legacy.pitches[p] = desc.layers[0].pitch[p];
                legacy.offsets[p] = desc.layers[0].offset[p];
            }
            legacy.buffers = &fd;
            legacy.num_buffers = 1;
            bool prime1 = !std::strcmp(mode, "prime1");
            VASurfaceAttrib imported[2]{};
            imported[0].type = VASurfaceAttribMemoryType;
            imported[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
            imported[0].value.type = VAGenericValueTypeInteger;
            imported[0].value.value.i = prime1 ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME
                                               : VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
            imported[1].type = VASurfaceAttribExternalBufferDescriptor;
            imported[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
            imported[1].value.type = VAGenericValueTypePointer;
            imported[1].value.value.p = prime1 ? static_cast<void *>(&legacy) : &desc;
            result = destroy_surfaces(display, &surfaces[i], 1);
            if (result == VA_STATUS_SUCCESS)
                result =
                    create_surfaces(display, format, width, height, &surfaces[i], 1, imported, 2);
        }
        close_objects(desc);
        if (result != VA_STATUS_SUCCESS)
            return result;
        std::lock_guard<std::mutex> lock(mutex);
        storage[surfaces[i]] = {info.st_dev, info.st_ino};
    }
    return result;
}
VAStatus vaSyncSurface(VADisplay display, VASurfaceID surface) {
    auto sync = reinterpret_cast<decltype(&vaSyncSurface)>(dlsym(RTLD_NEXT, "vaSyncSurface"));
    VAStatus result = sync(display, surface);
    if (result != VA_STATUS_SUCCESS)
        return result;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = storage.find(surface);
    if (it == storage.end())
        return result;
    VADRMPRIMESurfaceDescriptor desc{};
    result = describe(display, surface, desc);
    if (result != VA_STATUS_SUCCESS)
        return result;
    struct stat info{};
    bool same = desc.num_objects == 1 && !fstat(desc.objects[0].fd, &info) &&
                info.st_dev == it->second.device && info.st_ino == it->second.inode;
    close_objects(desc);
    if (!same) {
        std::fprintf(stderr, "FAIL: decoded surface changed its DMA-BUF\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    std::fprintf(stderr, "verified persistent surface=%u\n", surface);
    return result;
}
VAStatus vaDestroySurfaces(VADisplay display, VASurfaceID *surfaces, int count) {
    VAStatus result = destroy_surfaces(display, surfaces, count);
    if (result == VA_STATUS_SUCCESS) {
        std::lock_guard<std::mutex> lock(mutex);
        for (int i = 0; i < count; ++i)
            storage.erase(surfaces[i]);
    }
    return result;
}
