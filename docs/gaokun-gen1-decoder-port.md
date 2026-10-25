# Gaokun3 HFI Gen1 解码适配评估

本文记录 `libva-v4l2` 在 Huawei Gaokun3 / SC8280XP、Iris HFI Gen1
固件上的适配边界、已知缺口和实施顺序，作为后续开发的持久化依据。

## 范围约束

本项目只适配硬件视频解码，完全不做编码。

明确不在范围内的工作包括：

- H.264、HEVC 或其他格式的硬件编码；
- Gen1 encoder V4L2 控件、HFI property 或用户态兼容处理；
- 编码质量、码率控制、GOP、关键帧、SPS/PPS 和 deblocking 验证；
- Sunshine 编码或硬件转码输出验证；
- 为了编码而修改内核、固件接口或 `libva-v4l2`。

即使上游项目包含 encoder，Gaokun3 Gen1 适配也不得以“顺手补齐”或
“保持功能对称”为由扩大到编码。与编码有关的缺失控件和补丁不构成本项目的
阻塞项。

Gaokun3 用户态构建应显式使用：

```sh
meson setup build-gaokun-gen1 --buildtype=release \
  -Dgpu_copy=disabled -Dfastcv=disabled -Dgen1_decode_only=true
```

`gen1_decode_only` 不探测编码节点，只公布 H.264 Constrained Baseline/Main/High、
HEVC Main 和 VP9 Profile 0 的解码入口。CAPTURE pool 固定为 32；带超过 28 个
render target 的解码上下文会明确返回 `VA_STATUS_ERROR_MAX_NUM_EXCEEDED`，而不是
向 Gen1 firmware 请求 33–64 个 buffer。该选项默认关闭，避免改变 Q8B HFI
Gen2 的现有能力。

## 当前基线

- 用户态仓库：`radxa-pkg/libva-v4l2`
- 用户态基线：`6d03c3f8fb19b86f84b2b6c60ff20d1402f0ac41`
- 内核分支：`7.1.13-radxa-gaokun3-v4`
- 内核基线：`be7ab497e474`
- Gen1 decode-order 提交：`cb977e83ea4f`
- Gaokun3 固件：
  `/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn`
- 固件版本：
  `video-firmware.1.1-e736ad69a1da5570e3e99dd81cc5f55075111fe9`
- 固件 SHA-256：
  `77de47f764419559c762763c0c53a772541694c95ef337a6f23e15be94d6c5e2`

评估时远程内核工作区已有且必须保留的无关修改：

```text
arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dts
```

## 总体判断

Gen1 的 8-bit 解码适配可行性高。首个可用目标应限定为：

- H.264 Constrained Baseline、Main、High，8-bit；
- HEVC Main，8-bit；
- VP9 Profile 0，8-bit；
- 渐进式 4:2:0 视频。

首版不宣称支持：

- HEVC Main10；
- VP9 Profile 2；
- 10-bit P010/TP10；
- 超过 32 个 CAPTURE buffer 的工作负载；
- 4K、浏览器、长时间播放和任意 seek 的稳定性；
- 损坏码流的完整错误恢复。

旧的私有 `VIDIOC_IRIS_SURFACE_FENCE` ABI 不需要移植。当前
`libva-v4l2` 使用标准 DMA-BUF 隐式同步和 `poll()`，当前内核树中也没有应用
该私有补丁。

## 2026-10-25 实机进展

Gaokun3 已启动 Neutron clang/LLD 23 构建的
`7.1.13-gaokun3-chiyuki-iris-gen1`。Iris probe 明确报告 Gen1 firmware，
`/dev/video0` 的 decode-order 控件存在，内核日志没有 Iris 错误。

使用 `-Dgen1_decode_only=true -Dgpu_copy=disabled -Dfastcv=disabled` 的
用户态构建完成了以下 VA-API 硬解与软件解码逐帧 `framemd5` 对比：

| 格式 | 测试流 | 帧数 | 结果 |
| --- | --- | ---: | --- |
| H.264 High 8-bit | 2560×1440、含 B 帧 | 120 | 完全一致 |
| HEVC Main 8-bit | 640×360、含 B 帧 | 120 | 完全一致 |
| VP9 Profile 0 8-bit | 421×868 | 120 | 完全一致 |

受限构建的 `vainfo` 只公布 H.264 Constrained Baseline/Main/High、HEVC Main
和 VP9 Profile 0 的 `VAEntrypointVLD`，不公布编码、HEVC Main10 或 VP9
Profile 2。`tests/decoder/gen1-capabilities.cpp` 也验证了 Main10、编码入口和
第 29 个 render target 均被明确拒绝。驱动已安装为 `/usr/lib/dri/msm_drv_video.so`
alias；不设置环境变量的 FFmpeg 三 codec 回归通过，mpv 的 `vaapi-copy` 路径
连续完成 300 帧。

Wayland 显示路径有两种不同的 DMA-BUF 生命周期：

- `gpu-next` 请求 separate-layer 导出并逐帧重新导入，可以直接使用 Iris
  CAPTURE DMA-BUF，保持零拷贝；
- `dmabuf-wayland` 按 `VASurfaceID` 缓存 composed `wl_buffer`。Iris stateful
  decoder 每帧独立选择 CAPTURE buffer，因此 composed 导出必须固定到稳定 backing
  并复制像素；否则 surface 复用后会继续显示旧 DMA-BUF，表现为画面反复抽搐。

固定 backing 实现已通过同一段 2560×1440@60 H.264 视频的实机视觉验收；
`gpu-next` 与 `dmabuf-wayland` 两条硬解显示路径均不再抽搐。以上结果证明首个
decode-order 阻塞项和 composed 导出生命周期问题均已解除，但不扩大首版的
32-buffer、8-bit 和稳定性边界。

## 第一阻塞项：Gen1 decode-order

`src/decoder.cpp` 创建解码器时强制设置：

```text
V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY = 0
V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE = 1
```

当前 Gen1 Iris 没有注册这两个控件，因此用户态会在创建解码上下文时失败。

Gen1 有对应的旧 HFI ABI，当前同一内核树的 Venus 驱动也在使用：

```c
#define HFI_PROPERTY_PARAM_VDEC_OUTPUT_ORDER 0x1203005
#define HFI_OUTPUT_ORDER_DISPLAY             0x1000001
#define HFI_OUTPUT_ORDER_DECODE              0x1000002
```

不能使用 Gen2 的 `HFI_PROP_DECODE_ORDER_OUTPUT = 0x0300015b`，也不能复用
Gen2 的 TLV/port property 封包方式。

### 建议内核改动

预计只需修改 Gen1 Iris 的三个文件：

1. `iris_hfi_gen1_defines.h`
   - 加入 Gen1 output-order property 和枚举值。
2. `iris_hfi_gen1.c`
   - 给 Gen1 decoder capability 表加入 `DISPLAY_DELAY_ENABLE`；
   - 加入取值只能为零的 `DISPLAY_DELAY`；
   - 将 output-order property 加入 decoder OUTPUT stream 启动前的配置表。
3. `iris_hfi_gen1_command.c`
   - 增加该 property 的 Gen1 packet 序列化；
   - 将 V4L2 enable 值转换为 Gen1 枚举，而不是把 `0/1` 原样发送；
   - enable 为 1 时发送 `HFI_OUTPUT_ORDER_DECODE`；
   - enable 为 0 时发送 `HFI_OUTPUT_ORDER_DISPLAY`；
   - 只有客户端实际设置过 enable 控件时才覆盖固件默认值。

公共 `DISPLAY_DELAY` capability 枚举及 V4L2 control ID 映射已经随现有
Gen2 补丁进入内核树，不应重复添加。

这部分代码路径明确，但最终仍须通过有界实机测试确认 Gaokun3 Gen1 firmware
接受 `0x1203005`。如果固件拒绝该 property，备选方案将是把用户态改造成可预先
提交多帧的异步解码器；这不是首版方案。

## CAPTURE buffer 上限

用户态当前选择：

```text
pool_size = min(64, max(32, VA surface count + 4))
```

Gen1 内核当前：

- 默认最多提供 32 个 CAPTURE buffer；
- `HFI_PROPERTY_PARAM_BUFFER_COUNT_ACTUAL` 固定向固件发送 32；
- 部分 Gen1 数据结构仍以 `VIDEO_MAX_FRAME = 32` 为基准。

因此首版保持 32 个 buffer：

- 当 VA surface 数不超过 28 时，现有 `surfaces + 4` 可以落在 32 以内；
- 超过 28 个 surface 的客户端可能初始化失败；
- 不得直接照抄 Gen2 的 64-buffer vb2 修改并宣称完成；
- 不得仅在用户态静默截断为 32，因为客户端长期占用过多 surface 时可能死锁。

扩大到 64 必须作为独立解码工作项验证：

1. vb2 CAPTURE 上限；
2. Gen1 `BUFFER_COUNT_ACTUAL` 的实际数量；
3. firmware 是否接受 33 至 64；
4. VLC/frame threading、高参考帧数、seek 和长时间回收行为。

## 错误缓冲区时间戳

上游 `0004-media-iris-Preserve-timestamps-on-error-buffers.patch` 对 Gen1
方向正确，但单独应用可能不完整。

Gen1 response 当前仅在 `filled_len != 0` 时提取 firmware timestamp。对于
payload 为空的 corrupt/drop frame，公共 buffer-done 层即使改成保留
`buf->timestamp`，仍可能只保留到零。

完整的 Gen1 解码修复应满足：

- 非 EOS 的错误输出即使 payload 为空，也读取有效 firmware timestamp；
- 公共 buffer-done 路径不得再次清零；
- EOS/LAST 空缓冲区继续保持原语义；
- 用户态能够用 timestamp 找到对应 pending picture 并完成错误恢复。

该修复不阻塞无错误码流的首轮验证，但属于达到稳定可用所需的第二阶段工作。

## 10-bit 解码

10-bit 不是简单增加一个格式枚举。当前已知缺口包括：

- Gen1 sequence-change 对非零 `event.bit_depth` 直接报 unsupported；
- Gen1 raw-format setter 没有完整选择 P010/TP10；
- stride、plane constraint、buffer size 和 firmware 输出格式需要成套匹配；
- P010 枚举存在不代表实际解码可用。

固件二进制包含 P010/TP10 相关字符串，但也包含：

```text
Unsupported property - HFI_PROPERTY_PARAM_VDEC_PIXEL_BITDEPTH
```

此前 VP9 Profile 2 测试为零输出帧。首版必须隐藏或明确拒绝 HEVC Main10 和
VP9 Profile 2，不能让 `vainfo` 把格式枚举误报成可用能力。

长期更合理的做法是：

- 内核只枚举真正可用的 Gen1 CAPTURE 格式；
- 用户态根据 V4L2 codec/CAPTURE format 动态公布 VA profile；
- 不引入硬编码的产品名或临时“Gen1 全功能”开关。

## 分阶段实施

### 阶段 1：最小 8-bit 解码

- 实现 Gen1 decode-order 控件和 HFI property；
- 保持 32-buffer 上限；
- 不修改 10-bit；
- 不修改任何编码路径；
- 使用 Neutron 工具链、`ARCH=arm64 LLVM=1` 完成编译验证。

### 阶段 2：有界实机验收

在另行获得安装和硬件测试授权后：

- 每种格式先解码约 20 帧；
- 测试 H.264 Baseline/Main/High，High 样本必须含 B-frame；
- 测试 HEVC Main 8-bit；
- 测试 VP9 Profile 0；
- 将硬件输出和软件解码 `framemd5` 比较；
- 记录 firmware 是否接受 output-order property；
- 不在首轮执行长时间、4K 或并发压力测试。

### 阶段 3：解码稳定性

- 修复 Gen1 错误时间戳；
- 验证 EOS、flush、重复 seek 和动态分辨率变化；
- 评估 32-buffer 对实际播放器的限制；
- 决定是否单独实验 64-buffer。

### 阶段 4：可选的 10-bit 解码

只有 8-bit 稳定后才开始，且仍然只做解码。必须将 bit-depth event、
raw format、buffer layout 和用户态 capability probing 作为一个整体设计和
验收。

## 验收标准

“Gen1 解码首版完成”必须同时满足：

- 内核补丁不改变 Gen2 行为；
- Neutron 完整编译通过；
- H.264、HEVC Main 和 VP9 Profile 0 的选定样本可完成有界解码；
- 对比范围内的 `framemd5` 与软件解码一致；
- 不依赖私有 surface-fence ioctl；
- 不误报 10-bit 已可用；
- 不包含任何编码适配、编码测试或编码相关顺手修改；
- 不覆盖或混入远程 BOE DTS 既有改动。

编译通过只证明代码可链接，不等于 firmware 接受 property，也不等于实机解码
验收通过。安装、加载模块和硬件运行测试是独立授权步骤。
