# Gaokun3 Vulkan VPP移植与验证

## 范围

2026-11-21在Gaokun3、`7.2.15-gaokun3-chiyuki+`、Adreno 690、
Mesa Turnip 26.2.6上验证。驱动保留HFI Gen1解码、32个CAPTURE缓冲区上限、
768MiB输出池预算和28个解码render target限制，不开放编码或AV1。

移植上游VPP重构、共享滤波与表面管理依赖，以及Vulkan色调映射、
缩放、旋转和叠加合成提交，保留来源提交边界、作者、标题及来源哈希。
CDSP相关源码仅作为上游依赖保留，默认禁用；未下载、构建或验证
Hexagon工具链及CDSP模块。跳过CDSP专用性能优化、AV1和项目改名。

## 构建

```sh
meson setup build-gaokun-vulkan --buildtype=release --wrap-mode=nodownload \
  -Dgen1_decode_only=true -Dgpu_copy=disabled -Dcdsp_vpp=disabled \
  -Dvulkan_vpp=enabled -Dvpp_backend=vulkan -Dwerror=true
meson compile -C build-gaokun-vulkan
meson test -C build-gaokun-vulkan --print-errorlogs
```

Gen1的profile数组容量包含可选的VPP入口，避免新增
`VAProfileNone`时超出原有七个解码profile的容量。

## 实机验证

使用未安装的构建，通过`LIBVA_DRIVER_NAME=v4l2`和
`LIBVA_DRIVERS_PATH="$PWD/build-gaokun-vulkan"`选择驱动。
`IRIS_VAAPI_VPP=vulkan`明确选择Vulkan后端，
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json`
限定使用Turnip，不允许llvmpipe代替GPU。

以下测试使用`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`，
日志没有Vulkan validation错误或警告：

- `tests/vpp/scaling.cpp`：NV12/P010缩放、裁剪、位深转换、旋转、
  填充布局、PRIME身份和导入生命周期均通过。
- `tests/vpp/tonemap.cpp --geometry`：HDR10→SDR与双精度参考对比通过，
  最大像素值误差为1；色调映射、缩放与旋转组合通过。
- `tests/vpp/overlay.cpp vulkan --geometry`：BGRA/RGBA、
  NV12/P010叠加、透明度、裁剪、多层合成、拒绝非法参数和组合缩放通过。
- 三项CAPTURE预算及MSM缓存模式host测试通过。
- 无硬件Gen1 API回归通过：七个解码profile、禁用编码、
  28个解码render target限制保持不变，另加VPP入口。
- 经libva加载的Gen1能力回归通过，VPP不改变原有解码入口及限制。
- 普通用户运行FFmpeg的H.264硬解→Vulkan缩放→下载帧链路通过：
  320×192缩放为160×128，12帧完整输出，无解码错误。
- H.264和VP9的320×192、12帧短流硬解输出与软件解码逐帧MD5完全一致。
- 关闭Vulkan/CDSP的纯解码构建及对应无硬件Gen1 API回归通过。

本轮没有新增HEVC逐帧解码对比：本机libx265在生成测试流时以
SIGILL退出。该问题不影响独立的P010/HDR VPP测试，未在本次任务中修复。

缩放和HDR测试自身需要直接分配system DMA heap输入缓冲区，
而本机该设备仅root可访问，因此以sudo运行测试程序；
不修改设备权限。叠加合成还以普通用户运行通过，验证
DMA heap不可访问时的coherent MSM GEM回退。

本记录不代表播放器自动采用VPP、长时间播放、功耗或挂起恢复已验收。
此轮验证不安装驱动、不推送，也不改变现有系统驱动和设备权限。
测试程序和详细日志位于忽略目录`tmp/vulkan-vpp/`。
