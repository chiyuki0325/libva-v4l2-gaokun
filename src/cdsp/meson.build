# The supplied Open Access package executes on Linux ARM64, independently of
# the host C/C++ compiler used to build the VA driver and FastRPC client.
if build_machine.system() != 'linux' or build_machine.cpu_family() != 'aarch64'
  error('cdsp_vpp requires a Linux ARM64 build machine (Open Access 19.0.02)')
endif
python = import('python').find_installation()
hexagon = subproject('hexagon').get_variable('hexagon_tools')

cc = meson.get_compiler('c')
# Some distributions ship libcdsprpc without a pkg-config file.
fastrpc_include_dir = get_option('fastrpc_include_dir')
fastrpc_mode = get_option('fastrpc')
fastrpc_dep = dependency('', required: false)
if fastrpc_mode != 'bundled' and import('fs').is_dir(fastrpc_include_dir)
  rpc_library = cc.find_library('cdsprpc', required: false)
  rpc_includes = include_directories(fastrpc_include_dir, is_system: true)
  if rpc_library.found() and cc.links('''
#include <remote.h>
#include <AEEStdErr.h>
int main(void) {
  fastrpc_capability capability = {CDSP_DOMAIN_ID, ARCH_VER, 0};
  remote_handle_control(DSPRPC_GET_DSP_INFO, &capability, sizeof(capability));
  capability.attribute_ID = HVX_SUPPORT_128B;
  remote_handle_control(DSPRPC_GET_DSP_INFO, &capability, sizeof(capability));
  remote_register_buf(0, 0, -1);
  fastrpc_mmap(CDSP_DOMAIN_ID, -1, 0, 0, 0, FASTRPC_MAP_STATIC);
  fastrpc_munmap(CDSP_DOMAIN_ID, -1, 0, 0);
  return remote_session_control(0, 0, 0);
}''', dependencies: rpc_library, include_directories: rpc_includes, name: 'FastRPC mapping and architecture query APIs')
    fastrpc_dep = declare_dependency(dependencies: rpc_library, include_directories: rpc_includes)
  endif
endif
if not fastrpc_dep.found()
  if fastrpc_mode == 'system'
    error('Compatible system FastRPC headers and libcdsprpc are required')
  endif
  rpc_project = subproject('fastrpc')
  fastrpc_dep = rpc_project.get_variable('fastrpc_dep')
  fastrpc_include_dir = rpc_project.get_variable('fastrpc_include_dir')
  summary('FastRPC', 'pinned source fallback')
else
  summary('FastRPC', 'system')
endif

foreach arch : ['v68', 'v69', 'v73', 'v75', 'v79', 'v81']
  custom_target('iris-vpp-dsp-' + arch,
    output: 'vpp_' + arch + '_skel.so',
    command: [python, files('build-dsp.py'), arch, hexagon, fastrpc_include_dir,
              meson.current_source_dir(), '@OUTPUT@'],
    depend_files: files('iris_vpp.h', 'iris_vpp_rpc.h', 'iris_vpp_skel.c',
                       'dsp.c', 'overlay.h', '../overlay-params.h', 'filter.h',
                       'qurt_abi.h', 'qurt_abi_checks.h', 'vpp_limits.h'),
    build_by_default: true, install: true, install_dir: get_option('cdsp_firmware_dir'))
endforeach
skel_args = ['-DIRIS_VPP_DSP_DIR="' + (get_option('prefix') / get_option('cdsp_firmware_dir')) + '"']

# Keep FastRPC out of the VA driver's DT_NEEDED list: decoding and encoding
# remain available when the optional VPP runtime cannot be loaded.
vpp_client = shared_library('iris_vpp', 'client.cpp', 'skel.cpp', 'iris_vpp_stub.c',
  dependencies: [fastrpc_dep, threads],
  cpp_args: skel_args,
  install: true, install_dir: get_option('libdir') / 'libva-v4l2',
  install_rpath: '$ORIGIN')

# Mock only the FastRPC boundary; exercise the production selector without a DSP.
skel_test = executable('vpp-skel-selection', '../../tests/vpp/skel-selection.cpp',
  'skel.cpp',
  dependencies: fastrpc_dep.partial_dependency(includes: true),
  include_directories: include_directories('.'),
  cpp_args: skel_args, build_by_default: false)
test('CDSP skeleton selection', skel_test)

rpc_test = executable('vpp-rpc', '../../tests/vpp/rpc.c', '../../tests/vpp/rpc-skel.c',
  'iris_vpp_stub.c',
  dependencies: fastrpc_dep.partial_dependency(includes: true),
  include_directories: include_directories('.'), build_by_default: false)
test('CDSP RPC wire ABI', rpc_test)
