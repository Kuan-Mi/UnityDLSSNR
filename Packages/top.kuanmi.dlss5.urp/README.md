# UnityRHI DLSS 5 Neural Rendering for URP

A standalone, full-resolution DLSS Neural Rendering post-process for Unity 6.3
URP. It consumes the rasterized camera color, depth and motion vectors. It does
not depend on RTX Path Tracing, DLSS Ray Reconstruction, super resolution or
frame generation.

## Requirements

- Unity 6.3 with URP 17 or newer and RenderGraph enabled.
- Windows x64 using Direct3D 12.
- `top.kuanmi.unityrhi` and the packed `top.kuanmi.unityrhi.native` package.
- NVIDIA hardware and driver supported by the bundled DLSS-NR runtime. The
  current native integration reports availability through
  `RhiCore.IsDlssNrAvailable`.

## Setup

1. Build and pack the native UnityRHI package from the repository root, then
   copy the packed package to the target project's
   `Packages/top.kuanmi.unityrhi.native` folder. It must be an embedded package
   under `Packages` because its preload function must run before Unity creates
   the D3D12 device.
2. Add `top.kuanmi.unityrhi` and this package to the target project. Do not
   reference `top.kuanmi.unityrhi.native` directly from its external `Build`
   folder; use the copy under the target project's `Packages` folder.
3. Enable Direct3D 12, then restart the Editor.
4. Open the Universal Renderer Data used by the active URP asset.
5. Select **Add Renderer Feature > DLSS Neural Rendering**.
6. Keep the pass at **After Rendering Post Processing**.
7. Add a global or local Volume, create or select its Volume Profile, choose
   **Add Override > Post-processing > DLSS Neural Rendering**, override
   **Enabled**, and turn it on.

The feature requests URP depth and motion-vector textures automatically. Image
controls are read from the current camera's blended Volume stack, so they can
be animated or adjusted in Play Mode without modifying and reimporting the URP
Renderer Data asset. It only processes the final camera in a camera stack by
default, so overlays are included once and temporal history is not evaluated
more than once per frame.

---

# 中文翻译

# UnityRHI DLSS 5 神经渲染（URP）

这是一个面向 Unity 6.3 URP、独立运行且保持完整分辨率的 DLSS 神经渲染后处理。它使用光栅化后的相机颜色、深度和运动矢量，不依赖 RTX 路径追踪、DLSS 光线重建、超分辨率或帧生成。

## 要求

- Unity 6.3，使用 URP 17 或更高版本，并启用 RenderGraph。
- Windows x64，使用 Direct3D 12。
- `top.kuanmi.unityrhi` 和已打包的 `top.kuanmi.unityrhi.native` 包。
- NVIDIA 硬件和驱动须受随附的 DLSS-NR 运行时支持。当前原生集成通过 `RhiCore.IsDlssNrAvailable` 报告可用性。

## 设置

1. 从仓库根目录构建并打包原生 UnityRHI 包，然后将打包后的包复制到目标项目的 `Packages/top.kuanmi.unityrhi.native` 文件夹中。该包必须作为嵌入式包放在 `Packages` 下，因为其中的预加载函数必须在 Unity 创建 D3D12 设备之前执行。
2. 将 `top.kuanmi.unityrhi` 和本包添加到目标项目。不要从外部 `Build` 文件夹直接引用 `top.kuanmi.unityrhi.native`；请使用目标项目 `Packages` 文件夹中的副本。
3. 启用 Direct3D 12，然后重启编辑器。
4. 打开当前 URP 资源所使用的 Universal Renderer Data。
5. 选择 **Add Renderer Feature > DLSS Neural Rendering**。
6. 将渲染阶段保持为 **After Rendering Post Processing**。
7. 添加一个全局或局部 Volume，创建或选择其 Volume Profile，然后选择 **Add Override > Post-processing > DLSS Neural Rendering**，勾选 **Enabled** 的覆盖选项并将其开启。

该功能会自动请求 URP 深度纹理和运动矢量纹理。图像控制参数从当前相机混合后的 Volume 栈中读取，因此可在播放模式下制作动画或调整，而无需修改并重新导入 URP Renderer Data 资源。默认情况下，它只处理相机堆栈中的最终相机，因此叠加相机只会被合成一次，时间历史也不会在一帧内重复求值。
