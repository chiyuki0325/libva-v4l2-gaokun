# libva-v4l2

Experimental VA-API backend for Qualcomm Iris via Linux V4L2 stateful M2M,
providing hardware decoding and encoding on **SC8280XP**, tested on
**Radxa Dragon Q8B**. This is not a generic backend for all V4L2 devices.

## Support

| Codec | Decoding | Encoding |
| --- | --- | --- |
| H.264 / AVC | Constrained Baseline, Main, High (8-bit) | Same profiles (8-bit) |
| H.265 / HEVC | Main, Main10 (8/10-bit) | Main (8-bit) |
| VP9 | Profiles 0 and 2 (8/10-bit) | — |

- Progressive 4:2:0 video only. Decoding tested with FFmpeg, GStreamer, mpv,
  VLC, Chromium, and Kodi; encoding with FFmpeg, GStreamer and Sunshine.
- Encoding: CQP/CBR/VBR, even dimensions from 128×128 to 3840×2160, I/P frames
  with one reference; no B frames. I-frame requests produce IDR frames.
  HEVC uses Main tier; Main10 encoding is unsupported.
- Optional FastCV VPP scales linear NV12 on CDSP with DMA-BUF support;
  widths must be multiples of 16 and heights even. No color/bit-depth
  conversion or other VPP filters.
- Compatible DMA-BUF paths avoid raw-frame copies; cached surfaces may need
  GPU or CPU copying. PRIME1/PRIME2 import accepts single-buffer linear
  NV12/P010; export uses PRIME2.
- Identified decode errors produce gray frames with the Iris error-timestamp
  patch below; stalls remain errors.

## Performance

HEVC Main (8-bit) average FPS on **Radxa Dragon Q8B — SC8280XP, Iris HFI Gen2**,
using FFmpeg 9.0.1 and GStreamer 1.28.6.

| Pipeline | Operation | 720p (1280×720) | 1080p (1920×1080) | 4K (3840×2160) |
| --- | --- | ---: | ---: | ---: |
| FFmpeg VA-API | Decode | 1111 | 806 | 297 |
| FFmpeg VA-API | Encode | 1289 | 714 | 206 |
| GStreamer VA | Decode | 1088 | 823 | 278 |
| GStreamer VA | Encode | 1285 | 709 | 206 |
| GStreamer direct V4L2 | Decode | 1199 | 882 | 330 |
| GStreamer direct V4L2 | Encode | 1287 | 724 | 210 |

Synthetic I/P streams, GOP 60, VBR at 5/10/20 Mbps respectively. Encoding includes
CPU NV12 upload; FFmpeg uses **`async_depth=4`**. Decoding excludes CPU download
and rendering. Three-run averages, except 4K encoding (one run); actual performance
depends on content and pipeline.

## Requirements

- Linux with Qualcomm Iris stateful and MSM DRM drivers, plus access to the
  Iris video devices and DRM render node.
- Kernel changes in [patches/](patches/), unless already present: decode-order
  output, larger CAPTURE pools, encoder deblocking controls, and error timestamps.
- C++17 compiler, Meson ≥ 0.61, Ninja, pkg-config, libva ≥ 1.10 and libdrm
  development libraries. GPU-copy also requires EGL, GLES 3.2, and GBM.
- For VPP: FastCV and FastRPC libraries with a working CDSP runtime.

## Build and install

On Arch Linux:

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf libva libdrm mesa libglvnd
python3 packaging/make-dist.py
cd packaging/arch
makepkg -si
```

Regenerate the archive after editing packaged sources. Alternatively, use Meson:

```sh
meson setup build --buildtype=release --prefix=/usr -Dlibdir=lib
meson compile -C build
sudo meson install -C build
```

- `-Dgpu_copy=auto` (default): enable GPU copying when dependencies are available;
  `disabled` uses CPU copying when needed, `enabled` requires dependencies.
  DMA-BUF zero-copy paths are unaffected. Distribution packages enable GPU-copy.
- `-Dfastcv=disabled` (default): omit VPP; `auto` enables it when dependencies
  are available, `enabled` requires them. Missing runtime libraries disable
  only VPP. Debian/Ubuntu packages disable FastCV.

Adjust `libdir` if drivers belong somewhere other than `/usr/lib/dri`.
The installed MSM alias selects this backend automatically on the target platform.
Check installation with `vainfo` (`libva-utils` on Arch):

```sh
vainfo --display drm --device /dev/dri/renderD128
```

## Usage

Playback:

```sh
mpv --hwdec=vaapi input.mp4
```

H.264 encoding (for HEVC, replace the codec/profile with `hevc_vaapi`/`main`):

```sh
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 -an \
  -vf format=nv12,hwupload -c:v h264_vaapi -profile:v high \
  -bf 0 -g 60 -rc_mode CBR -b:v 6M -async_depth 4 output.mp4
```

Hardware transcoding of supported 8-bit input:

```sh
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i input.mp4 -an \
  -c:v h264_vaapi -bf 0 -qp 24 -async_depth 4 output.mp4
```

- With FastCV VPP enabled, add `-vf scale_vaapi=w=1920:h=1080` to the
  transcoding command to scale decoded NV12 on CDSP.
- MP4 and raw H.264 output are supported; for Matroska, encode to raw H.264
  first and remux. FFmpeg packed-header warnings are expected because firmware
  produces the headers. Custom VUI/SEI metadata is not forwarded.
- GStreamer: set `GST_VA_ALL_DRIVERS=1` to enable this driver's `va` elements.
- Sunshine/Moonlight: use **H.264 / SDR**; HEVC/AV1 probes may fail while
  H.264 remains available.

## Configuration and development

| Variable | Purpose |
| --- | --- |
| `LIBVA_DRIVER_NAME=v4l2` | Explicitly select this backend. |
| `LIBVA_DRIVERS_PATH=/path/to/build` | Load an uninstalled build. |
| `IRIS_VAAPI_DEVICE` / `IRIS_VAAPI_ENCODER_DEVICE` | Override video-device discovery. |
| `IRIS_VAAPI_COPY=gpu` or `cpu` | Default: GPU when built in, otherwise CPU. Explicit GPU requires GPU-copy support. |
| `IRIS_VAAPI_DEBUG=1` | Enable diagnostics. |

Use clang-format 22 with the checked-in `.clang-format` (Meson targets: `format`,
`format-check`). Hardware test instructions are in [tests/](tests/).

## License

[MIT](LICENSE) for the backend; GPL-2.0-only for bundled kernel patches.
External test frameworks and media retain their own licenses.
