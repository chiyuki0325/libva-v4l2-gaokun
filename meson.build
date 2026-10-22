project(
  'libva-v4l2', 'cpp', 'c',
  version: '1.0.0',
  meson_version: '>=0.61.0',
  license: 'MIT',
  default_options: ['cpp_std=c++17', 'warning_level=2'],
)

va = dependency('libva', version: '>=1.10')
drm = dependency('libdrm')
threads = dependency('threads')
gpu_copy_option = get_option('gpu_copy')
egl = dependency('egl', required: gpu_copy_option)
gles = dependency('glesv2', version: '>=3.2', required: gpu_copy_option)
gbm = dependency('gbm', required: gpu_copy_option)
have_gpu_copy = egl.found() and gles.found() and gbm.found()
copy_dependencies = []
copy_source = files('src/cpu-copy.cpp')
if have_gpu_copy
  copy_dependencies = [egl, gles, gbm]
  copy_source = files('src/gpu-copy.cpp')
endif
summary('GPU surface copy', have_gpu_copy)

sources = files(
  'src/backend.cpp',
  'src/decoder.cpp',
  'src/encoder.cpp',
  'src/h264.cpp',
  'src/hevc.cpp',
  'src/msm.c',
  'src/surface-copy.cpp',
  'src/vp9.cpp',
  'src/vpp.cpp',
)
headers = files('src/internal.hpp')
driver_install_dir = get_option('libdir') / 'dri'

shared_library(
  'v4l2_drv_video',
  sources + copy_source,
  dependencies: [va, drm, threads] + copy_dependencies,
  name_prefix: '',
  install: true,
  install_dir: driver_install_dir,
)

# libva falls back to the DRM driver name on Qualcomm MSM devices.
install_symlink(
  'msm_drv_video.so',
  pointing_to: 'v4l2_drv_video.so',
  install_dir: driver_install_dir,
  install_tag: 'runtime',
)

install_data('README.md', install_dir: get_option('datadir') / 'doc' / meson.project_name())
install_data('LICENSE', install_dir: get_option('datadir') / 'licenses' / meson.project_name())

formatter = find_program('clang-format', required: false)
if formatter.found()
  sources += files('src/cpu-copy.cpp', 'src/gpu-copy.cpp')
  run_target('format', command: [formatter, '-i', '--style=file', sources, headers])
  run_target('format-check', command: [formatter, '--dry-run', '--Werror', '--style=file', sources, headers])
endif
