# UnityRHI DLSS for URP

DLSS SuperSampling, DLSS Frame Generation, and DLSS 5 Neural Rendering for Unity 6.3 URP, driven by
UnityRHI's native NGX plugin.

| Feature | Integration | What it does |
|---|---|---|
| **DLSS Super Resolution** | URP **IUpscaler** | DLAA and quality-mode super resolution (Quality / Balanced / Performance / Ultra Performance). XR uses a separate NGX instance per eye. |
| **DLSS Frame Generation** | Renderer Feature | Player-only interpolated frames at Present from depth, motion and the swap-chain color. Works at native resolution or after Super Resolution. No XR. No Volume. |
| **DLSS Neural Rendering** | Renderer Feature + Volume | Full-resolution SDR neural pass over raster color, depth and motion. No RTX Path Tracing, Ray Reconstruction or frame generation. |

## Requirements

- Unity 6.3 with URP 17+ and RenderGraph enabled.
- Super Resolution also needs the scripting define `ENABLE_UPSCALER_FRAMEWORK`.
- Windows x64 using Direct3D 12.
- `top.kuanmi.unityrhi` and packed `top.kuanmi.unityrhi.native` (`nvngx_dlss.dll` for SR, `nvngx_dlssg.dll` for FG, `nvngx_dlssnr.dll` for NR).
- NVIDIA hardware/driver that exposes the corresponding NGX feature (`RhiCore.IsNgxDlssAvailable` / `RhiCore.IsNgxFrameGenerationAvailable` / `RhiCore.IsDlssNrAvailable`).

## Super Resolution setup

1. Install `top.kuanmi.unityrhi` / `top.kuanmi.unityrhi.native`. Enable Direct3D 12, restart the Editor.
2. Add this package. Ensure **Player Settings → Scripting Define Symbols** includes `ENABLE_UPSCALER_FRAMEWORK`.
3. On the active **URP Asset**:
   - Upscaling Filter = **IUpscaler**
   - Active upscaler = **UnityRHI DLSS**
   - Render Scale = **1**
   - MSAA = **Off**
4. Open the UnityRHI DLSS upscaler options on the URP Asset and pick a quality mode and NGX preset.
5. Disable camera Temporal Anti-Aliasing (IUpscaler owns jitter when temporal).
   Super Resolution has no Volume override; quality and preset live only on the URP Asset.

Motion vectors and depth are taken from URP at the negotiated render resolution.
Optimal render size comes from `NGX_DLSS_GET_OPTIMAL_SETTINGS`
(`RhiCore.QueryDlssOptimalSettings`). Scene view stays native resolution; only
Game cameras negotiate lower pre-upscale sizes. XR keeps per-eye history
(multipass or single-pass instanced texture arrays).

## Frame Generation setup

Frame Generation is **Player-only**. The native Present hook does not insert
frames in the Editor Game view. Build a standalone Windows x64 player.

1. Open the Universal Renderer Data used by the active URP asset.
2. Select **Add Renderer Feature > DLSS Frame Generation**.
3. Keep the pass at **After Rendering Post Processing**. Depth and motion are
   copied for the native Present path; interpolated color comes from the swap
   chain. Enable **Enable Frame Generation** on the feature (F8 in a Player
   build also toggles it). There is no Volume override.

The feature requests URP depth and motion-vector textures automatically. While
it is active it clears VSync and `Application.targetFrameRate` (NVIDIA frame
pacing). It only processes the final Game camera in a camera stack. XR and
cameras that render to a `targetTexture` are skipped.

Depth/MV stay at render resolution, so Frame Generation can stack with UnityRHI
DLSS Super Resolution. The interpolated frames are generated from the displayed
backbuffer after post-processing.

## Neural Rendering setup

1. Open the Universal Renderer Data used by the active URP asset.
2. Select **Add Renderer Feature > DLSS Neural Rendering**.
3. Keep the pass at **Before Rendering Post Processing** (NR → remaining post / SR).
   Use **After Rendering Post Processing** only for the optional post → NR stack.
4. Add a global or local Volume, create or select its Volume Profile, choose
   **Add Override > Post-processing > DLSS Neural Rendering**, override
   **Enabled**, and turn it on.

The feature requests URP depth and motion-vector textures automatically. Image
controls are read from the current camera's blended Volume stack. It only
processes the final camera in a camera stack by default. XR is evaluated per
eye at native resolution.

## Stacking Super Resolution and Neural Rendering (SDR)

Default Game-camera order: **raster → NR → IUpscaler → remaining post**.

Set NR's Render Pass Event to **Before Rendering Post Processing** so NR runs
at the negotiated render resolution. Color, depth and motion then match, and
IUpscaler upscales the denoised image before bloom / tonemap.

Optional reverse stack: **After Rendering Post Processing** (SR → post → NR).
When color is already display-sized and depth/motion are still render-sized,
the NR prepare pass resamples depth/MV into NR inputs.

Frame Generation runs after post (`After Rendering Post Processing`) and reads
the same render-resolution depth/MV. Displayed FPS with FG enabled is
`RhiCore.DisplayedPresentCount`, not `Time.deltaTime`.

---

# 中文翻译

# UnityRHI DLSS（URP）

通过 UnityRHI 原生 NGX 在 Unity 6.3 URP 中提供 DLSS SuperSampling、DLSS 帧生成与 DLSS 5 神经渲染。

| 功能 | 接入方式 | 作用 |
|---|---|---|
| **DLSS 超分辨率** | URP **IUpscaler** | DLAA 与各质量档超分。XR 左右眼各自一份 NGX 历史。 |
| **DLSS 帧生成** | Renderer Feature | 仅 Player：在 Present 时插入插值帧。可用原生分辨率或叠在超分之后。不支持 XR。不使用 Volume。 |
| **DLSS 神经渲染** | Renderer Feature + Volume | 全分辨率 SDR 神经通道，使用光栅颜色/深度/运动矢量。不依赖路径追踪、光线重建或帧生成。 |

## 要求

- Unity 6.3，URP 17+，RenderGraph。
- 超分还需脚本宏 `ENABLE_UPSCALER_FRAMEWORK`。
- Windows x64 + D3D12。
- `top.kuanmi.unityrhi` 与 `top.kuanmi.unityrhi.native`。
- 对应 NGX 能力：`RhiCore.IsNgxDlssAvailable` / `RhiCore.IsNgxFrameGenerationAvailable` / `RhiCore.IsDlssNrAvailable`。

## 超分设置

1. 安装 UnityRHI 原生包，启用 D3D12 后重启。
2. 添加本包，并在 Player Settings 加入 `ENABLE_UPSCALER_FRAMEWORK`。
3. URP Asset：Upscaling Filter = **IUpscaler**，选择 **UnityRHI DLSS**，Render Scale = 1，MSAA Off。
4. 在 URP Asset 的 UnityRHI DLSS 选项里选择质量档和 NGX Preset。
5. 关闭相机 TAA。
   超分没有 Volume；质量和 Preset 只在 URP Asset 上设置。

Scene 视图保持原生分辨率；只有 Game 相机协商更低的预放大尺寸。XR 按眼保存历史（multipass 或 single-pass instanced）。

## 帧生成设置

帧生成**仅在独立 Player 中生效**。编辑器 Game 视图不会插入生成帧，需要打 Windows x64 包。

1. 打开当前 URP 资源所使用的 Universal Renderer Data。
2. 选择 **Add Renderer Feature > DLSS Frame Generation**。
3. 将渲染阶段保持为 **After Rendering Post Processing**。该通道只拷贝深度和运动矢量，插值颜色来自交换链 Present。在 Feature 上勾选 **Enable Frame Generation**（Player 里也可按 F8 切换）。没有 Volume。

开启后会关闭 VSync 并取消 `Application.targetFrameRate` 限制。默认只处理相机堆栈中的最终 Game 相机。XR 以及渲染到 `targetTexture` 的相机会被跳过。深度/运动矢量保持渲染分辨率，因此可以与 UnityRHI DLSS 超分叠加。带 FG 时的显示帧率请用 `RhiCore.DisplayedPresentCount`，不要用 `Time.deltaTime`。

## 神经渲染设置

1. 打开当前 URP 资源所使用的 Universal Renderer Data。
2. 选择 **Add Renderer Feature > DLSS Neural Rendering**。
3. 将渲染阶段保持为 **Before Rendering Post Processing**（NR → 其余后处理 / SR）。仅在需要后处理 → NR 时改为 After Rendering Post Processing。
4. 添加 Volume，选择 **Add Override > Post-processing > DLSS Neural Rendering**，勾选 **Enabled**。

## 与超分叠加（SDR）

默认 Game 相机顺序：**光栅 → NR → IUpscaler → 其余后处理**。

将 NR 的 Render Pass Event 设为 **Before Rendering Post Processing**，NR 在协商后的渲染分辨率上运行，随后 IUpscaler 放大，再做 Bloom / Tonemap。

可选反向：**After Rendering Post Processing**（SR → 后处理 → NR）。

帧生成在后处理之后运行，读取同一套渲染分辨率深度/运动矢量。开启 FG 后的显示帧率请用 `RhiCore.DisplayedPresentCount`。
