# UnityRHI DLSS Neural Rendering

UnityRHI is a Direct3D 12 render hardware interface and native plugin stack for
Unity. It records commands in C# and replays them directly on Unity's D3D12
command list, exposing ray tracing, bindless resources, compute, explicit
resource barriers, and NVIDIA neural rendering features.

This repository contains the managed UnityRHI package, its Windows x64 native
runtime, and a Unity 6.3 URP integration for DLSS Super Resolution and DLSS 5
Neural Rendering.

![DLSS Neural Rendering running in Unity](01.png)

## Repository layout

| Path | Description |
|---|---|
| `Packages/top.kuanmi.unityrhi` | UnityRHI C# runtime, editor tools, command stream, interop, and shader importer |
| `Packages/top.kuanmi.unityrhi.native` | Source layout for the Windows x64 native UPM package |
| `Packages/top.kuanmi.dlss.urp` | DLSS Super Resolution (IUpscaler) and DLSS 5 Neural Rendering for Unity 6.3 URP |
| `RenderingPlugin` | C++ sources and CMake project for UnityRHI, NRIPlugin, and supporting native libraries |
| `1-Deploy.bat` | Generates the Visual Studio 2022 x64 build files in `_Build` |
| `2-Build.bat` | Builds the native projects into `_Bin/<Configuration>` |
| `3-Pack.bat` | Creates the deployable native package under `Build/top.kuanmi.unityrhi.native` |
| `4-Clean.bat` | Removes generated build, binary, and package output directories |

## Requirements

- Windows x64.
- Unity 6.3 with Direct3D 12 selected as the active graphics API.
- Supported NVIDIA hardware and driver for the NVIDIA features being used.
- Unity 6.3 with URP 17 or newer and RenderGraph enabled for the DLSS URP
  package. Super Resolution also requires `ENABLE_UPSCALER_FRAMEWORK`.

Building the native package from source additionally requires CMake 3.24 or
newer, Visual Studio 2022 with the Desktop development with C++ workload, and
network access during initial CMake configuration.

## Download the prebuilt native package

The recommended option is to download
[`top.kuanmi.unityrhi.native-1.0.1.zip`](https://github.com/Kuan-Mi/UnityDLSSNR/releases/download/v1.0.1/top.kuanmi.unityrhi.native-1.0.1.zip)
from the [latest GitHub Release](https://github.com/Kuan-Mi/UnityDLSSNR/releases/latest).
It contains the prebuilt Windows x64 native package, so CMake, Visual Studio,
and the repository build scripts are not required.

Extract the archive directly into the target Unity project's `Packages`
folder. The resulting path must be:

```text
<UnityProject>/Packages/top.kuanmi.unityrhi.native/package.json
```

## Build the native package from source

Run the following scripts from the repository root in order:

```bat
1-Deploy.bat
2-Build.bat Release
3-Pack.bat Release
```

The packed native UPM package is written to:

```text
Build/top.kuanmi.unityrhi.native
```

Debug builds can be produced by passing `Debug` to the build and pack scripts.
Use `4-Clean.bat` to remove all generated output before a clean rebuild.

The NVIDIA-signed `nvngx_dlssnr.dll` is not provided by this repository. To use
DLSS Neural Rendering, obtain the DLL separately and copy it into the target
Unity project at:

```text
Packages/top.kuanmi.unityrhi.native/Plugins/x86_64/nvngx_dlssnr.dll
```

Without that DLL, the core UnityRHI native package can still be used, but DLSS
Neural Rendering will not be available.

## Add the packages to a Unity project

1. Download and extract the prebuilt native package into the target project's
   `Packages` folder, or copy the locally built
   `Build/top.kuanmi.unityrhi.native` directory there. The final path must be
   `Packages/top.kuanmi.unityrhi.native`.
2. Add or copy `Packages/top.kuanmi.unityrhi` into the target project.
3. For DLSS Super Resolution and/or Neural Rendering, also add or copy
   `Packages/top.kuanmi.dlss.urp`.
4. Select Direct3D 12 as the active Windows graphics API and restart the Unity
   Editor.

`top.kuanmi.unityrhi.native` must be an embedded package located inside the
target project's `Packages` folder. Do not reference the external packed
`Build` directory directly. The native package contains a preload function that
must run before Unity creates the D3D12 device.

After rebuilding the native code, replace the contents of the embedded
`Packages/top.kuanmi.unityrhi.native` package in the target project.

## Enable DLSS in URP

Super Resolution: on the URP Asset set Upscaling Filter to **IUpscaler** and
select **UnityRHI DLSS**. Add `ENABLE_UPSCALER_FRAMEWORK` to scripting defines.

Neural Rendering:

1. Open the Universal Renderer Data used by the active URP asset.
2. Select **Add Renderer Feature > DLSS Neural Rendering**.
3. Keep the pass at **Before Rendering Post Processing** (NR → remaining post / SR).
4. Add a global or local Volume and create or select its Volume Profile.
5. Select **Add Override > Post-processing > DLSS Neural Rendering**, override
   **Enabled**, and turn it on.

The renderer feature requests the required depth and motion-vector textures
automatically. Image controls are read from the active camera's blended Volume
stack and can be adjusted at runtime. Default SDR Game-camera order is
raster → NR → IUpscaler → remaining post.

## Package documentation

- [UnityRHI](Packages/top.kuanmi.unityrhi/README.md)
- [UnityRHI Native](Packages/top.kuanmi.unityrhi.native/README.md)
- [DLSS for URP](Packages/top.kuanmi.dlss.urp/README.md)

---

# 中文说明

# UnityRHI DLSS 神经渲染

UnityRHI 是面向 Unity 的 Direct3D 12 渲染硬件接口和原生插件栈。它在 C#
侧记录命令，并直接在 Unity 的 D3D12 命令列表上重放，从而提供光线追踪、
无绑定资源、计算、显式资源屏障和 NVIDIA 神经渲染能力。

本仓库包含 UnityRHI C# 包、Windows x64 原生运行时，以及面向 Unity 6.3 URP
的 DLSS 超分辨率与 DLSS 5 神经渲染集成。

## 仓库结构

| 路径 | 说明 |
|---|---|
| `Packages/top.kuanmi.unityrhi` | UnityRHI C# 运行时、编辑器工具、命令流、互操作层和着色器导入器 |
| `Packages/top.kuanmi.unityrhi.native` | Windows x64 原生 UPM 包的源目录结构 |
| `Packages/top.kuanmi.dlss.urp` | 面向 Unity 6.3 URP 的 DLSS 超分辨率（IUpscaler）与 DLSS 5 神经渲染 |
| `RenderingPlugin` | UnityRHI、NRIPlugin 和相关原生库的 C++ 源码及 CMake 工程 |
| `1-Deploy.bat` | 在 `_Build` 中生成 Visual Studio 2022 x64 构建文件 |
| `2-Build.bat` | 将原生项目构建到 `_Bin/<Configuration>` |
| `3-Pack.bat` | 在 `Build/top.kuanmi.unityrhi.native` 下生成可部署的原生包 |
| `4-Clean.bat` | 删除生成的构建目录、二进制文件和打包输出 |

## 环境要求

- Windows x64。
- Unity 6.3，并将 Direct3D 12 设为当前图形 API。
- 使用 NVIDIA 功能时，需要相应功能所支持的 NVIDIA 硬件和驱动。
- 使用 DLSS URP 包时，需要 Unity 6.3、URP 17 或更高版本，并启用 RenderGraph。超分还需脚本宏 `ENABLE_UPSCALER_FRAMEWORK`。

只有从源码构建原生包时，才额外需要 CMake 3.24 或更高版本、安装了“使用
C++ 的桌面开发”工作负载的 Visual Studio 2022，以及首次运行 CMake 配置时
用于下载锁定版本原生依赖项的网络连接。

## 下载预编译原生包

推荐直接从[最新 GitHub Release](https://github.com/Kuan-Mi/UnityDLSSNR/releases/latest)
下载
[`top.kuanmi.unityrhi.native-1.0.1.zip`](https://github.com/Kuan-Mi/UnityDLSSNR/releases/download/v1.0.1/top.kuanmi.unityrhi.native-1.0.1.zip)。
其中已经包含编译好的 Windows x64 原生包，无需安装 CMake、Visual Studio，
也无需运行仓库中的构建脚本。

将压缩包直接解压到目标 Unity 项目的 `Packages` 文件夹，最终路径必须为：

```text
<UnityProject>/Packages/top.kuanmi.unityrhi.native/package.json
```

## 从源码构建原生包

在仓库根目录中依次运行：

```bat
1-Deploy.bat
2-Build.bat Release
3-Pack.bat Release
```

打包后的原生 UPM 包位于：

```text
Build/top.kuanmi.unityrhi.native
```

向构建和打包脚本传入 `Debug` 可生成 Debug 版本。需要完全重新构建时，可先
运行 `4-Clean.bat` 删除所有生成内容。

本仓库不提供 NVIDIA 签名的 `nvngx_dlssnr.dll`。如需使用 DLSS 神经渲染，
请自行获取该 DLL，并将其复制到目标 Unity 项目的以下位置：

```text
Packages/top.kuanmi.unityrhi.native/Plugins/x86_64/nvngx_dlssnr.dll
```

缺少该 DLL 时仍可使用 UnityRHI 核心原生包，但 DLSS 神经渲染将不可用。

## 添加到 Unity 项目

1. 下载预编译原生包并解压到目标项目的 `Packages` 文件夹，或者将本地构建的
   `Build/top.kuanmi.unityrhi.native` 复制到该文件夹。最终路径必须为
   `Packages/top.kuanmi.unityrhi.native`。
2. 将 `Packages/top.kuanmi.unityrhi` 添加或复制到目标项目。
3. 如需使用 DLSS 超分辨率和/或神经渲染，还需添加或复制
   `Packages/top.kuanmi.dlss.urp`。
4. 将 Direct3D 12 设为 Windows 当前图形 API，然后重启 Unity 编辑器。

`top.kuanmi.unityrhi.native` 必须作为嵌入式包放在目标项目的 `Packages`
文件夹中，不能直接引用项目外已打包的 `Build` 目录。该原生包包含必须在
Unity 创建 D3D12 设备之前执行的预加载函数。

重新构建原生代码后，请替换目标项目中嵌入式
`Packages/top.kuanmi.unityrhi.native` 包的内容。

## 在 URP 中启用 DLSS

超分：在 URP Asset 上将 Upscaling Filter 设为 **IUpscaler**，并选择
**UnityRHI DLSS**。在脚本宏中加入 `ENABLE_UPSCALER_FRAMEWORK`。

神经渲染：

1. 打开当前 URP 资源所使用的 Universal Renderer Data。
2. 选择 **Add Renderer Feature > DLSS Neural Rendering**。
3. 将渲染阶段保持为 **Before Rendering Post Processing**（NR → 其余后处理 / SR）。
4. 添加一个全局或局部 Volume，并创建或选择其 Volume Profile。
5. 选择 **Add Override > Post-processing > DLSS Neural Rendering**，勾选
   **Enabled** 的覆盖选项并将其开启。

Renderer Feature 会自动请求所需的深度纹理和运动矢量纹理。图像控制参数从
当前相机混合后的 Volume 栈中读取，并可在运行时调整。默认 SDR Game 相机顺序为
光栅 → NR → IUpscaler → 其余后处理。

## 各包文档

- [UnityRHI](Packages/top.kuanmi.unityrhi/README.md)
- [UnityRHI Native](Packages/top.kuanmi.unityrhi.native/README.md)
- [DLSS（URP）](Packages/top.kuanmi.dlss.urp/README.md)
