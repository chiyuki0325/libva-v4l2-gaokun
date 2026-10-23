tonemap_spirv = custom_target(
  'tonemap_spirv',
  input: 'tonemap.comp',
  output: 'tonemap_spirv.h',
  command: [glslang, '-V', '--target-env', 'vulkan1.1', '--vn', 'tonemap_spirv',
            '-o', '@OUTPUT@', '@INPUT@'],
)
vulkan_sources = [files('vpp.cpp'), tonemap_spirv]
