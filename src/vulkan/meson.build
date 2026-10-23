tonemap_spirv = custom_target(
  'tonemap_spirv',
  input: 'tonemap.comp',
  output: 'tonemap_spirv.h',
  command: [glslang, '-V', '--target-env', 'vulkan1.1', '--vn', 'tonemap_spirv',
            '-o', '@OUTPUT@', '@INPUT@'],
)
overlay_spirv = custom_target(
  'overlay_spirv', input: 'overlay.comp', output: 'overlay_spirv.h',
  command: [glslang, '-V', '--target-env', 'vulkan1.1', '--vn', 'overlay_spirv',
            '-o', '@OUTPUT@', '@INPUT@'],
)
vulkan_sources = [files('vpp.cpp'), tonemap_spirv, overlay_spirv]
