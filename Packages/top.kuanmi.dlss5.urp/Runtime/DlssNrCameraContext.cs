using System;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using RhiTexture = UnityRhi.Texture;

namespace UnityRhi.DlssNr.Urp
{
    /// <summary>Persistent Unity/UnityRHI resources and NGX history for one camera.</summary>
    internal sealed class DlssNrCameraContext : IDisposable
    {
        internal readonly struct DispatchParameters
        {
            internal readonly bool Reset;
            internal readonly float MotionScaleX;
            internal readonly float MotionScaleY;
            internal readonly DlssNrPreset Preset;
            internal readonly DlssNrStyle Style;
            internal readonly float Intensity;
            internal readonly float LocalToneStrength;
            internal readonly float LocalStructureStrength;
            internal readonly float SkinStructureStrength;
            internal readonly bool UseAutoMask;
            internal readonly bool UiCorrection;

            internal DispatchParameters(bool reset, int width, int height, in DlssNrSettings settings)
            {
                Reset = reset;
                // URP stores previous-to-current motion in UV/NDC units. NGX consumes
                // current-to-previous motion; convert it to full-resolution pixels.
                MotionScaleX = -width * settings.MotionVectorScale.x;
                MotionScaleY = -height * settings.MotionVectorScale.y;
                Preset = settings.Preset;
                Style = settings.Style;
                Intensity = settings.Intensity;
                LocalToneStrength = settings.LocalToneStrength;
                LocalStructureStrength = settings.LocalStructureStrength;
                SkinStructureStrength = settings.SkinStructureStrength;
                UseAutoMask = settings.UseAutoMask;
                UiCorrection = settings.UiCorrection;
            }
        }

        internal int Width { get; }
        internal int Height { get; }
        internal RenderTexture ColorRt { get; private set; }
        internal RenderTexture MotionRt { get; private set; }
        internal RenderTexture DepthRt { get; private set; }
        internal RenderTexture OutputRt { get; private set; }
        internal RTHandle ColorHandle { get; private set; }
        internal RTHandle MotionHandle { get; private set; }
        internal RTHandle DepthHandle { get; private set; }
        internal RTHandle OutputHandle { get; private set; }

        private RhiTexture _color;
        private RhiTexture _motion;
        private RhiTexture _depth;
        private RhiTexture _output;
        private DlssNrContext _dlss;
        private CommandList _commandList;
        private int _lastFrame = int.MinValue;
        private int _lastSettingsHash;
        private Vector3 _lastPosition;
        private Quaternion _lastRotation;
        private Matrix4x4 _lastProjection;
        private bool _hasHistory;
        private bool _disposed;

        internal DlssNrCameraContext(int width, int height, string cameraName)
        {
            if (width <= 0) throw new ArgumentOutOfRangeException(nameof(width));
            if (height <= 0) throw new ArgumentOutOfRangeException(nameof(height));
            Width = width;
            Height = height;

            try
            {
                ColorRt = CreateRenderTexture($"DLSS-NR {cameraName} Color", width, height,
                    GraphicsFormat.R16G16B16A16_SFloat);
                MotionRt = CreateRenderTexture($"DLSS-NR {cameraName} Motion", width, height,
                    GraphicsFormat.R16G16_SFloat);
                DepthRt = CreateRenderTexture($"DLSS-NR {cameraName} Depth", width, height,
                    GraphicsFormat.R32_SFloat);
                OutputRt = CreateRenderTexture($"DLSS-NR {cameraName} Output", width, height,
                    GraphicsFormat.R16G16B16A16_SFloat);

                ColorHandle = RTHandles.Alloc(ColorRt);
                MotionHandle = RTHandles.Alloc(MotionRt);
                DepthHandle = RTHandles.Alloc(DepthRt);
                OutputHandle = RTHandles.Alloc(OutputRt);

                Device device = Device.Instance;
                // The prepare MRT leaves its attachments in RenderTarget state. The
                // DLSS command stream restores that state so the next URP frame starts
                // from the same contract. RenderGraph exposes Output as a UAV write.
                _color = Wrap(device, ColorRt, Format.RGBA16_FLOAT,
                    ResourceStates.RenderTarget, $"DLSS-NR {cameraName} Color");
                _motion = Wrap(device, MotionRt, Format.RG16_FLOAT,
                    ResourceStates.RenderTarget, $"DLSS-NR {cameraName} Motion");
                _depth = Wrap(device, DepthRt, Format.R32_FLOAT,
                    ResourceStates.RenderTarget, $"DLSS-NR {cameraName} Depth");
                _output = Wrap(device, OutputRt, Format.RGBA16_FLOAT,
                    ResourceStates.UnorderedAccess, $"DLSS-NR {cameraName} Output");
                _dlss = new DlssNrContext();
                _commandList = new CommandList(8);
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        internal DispatchParameters BeginFrame(Camera camera, int frameIndex, in DlssNrSettings settings)
        {
            bool reset = !_hasHistory || frameIndex != _lastFrame + 1 ||
                SettingsHash(settings) != _lastSettingsHash;
            if (_hasHistory)
            {
                if (Vector3.Distance(_lastPosition, camera.transform.position) > settings.CameraCutDistance ||
                    Quaternion.Angle(_lastRotation, camera.transform.rotation) > settings.CameraCutAngle ||
                    ProjectionChanged(_lastProjection, camera.nonJitteredProjectionMatrix))
                    reset = true;
            }

            _lastFrame = frameIndex;
            _lastSettingsHash = SettingsHash(settings);
            _lastPosition = camera.transform.position;
            _lastRotation = camera.transform.rotation;
            _lastProjection = camera.nonJitteredProjectionMatrix;
            _hasHistory = true;
            return new DispatchParameters(reset, Width, Height, settings);
        }

        internal void ResetHistory() => _hasHistory = false;

        internal void Record(CommandBuffer commandBuffer, in DispatchParameters parameters)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(DlssNrCameraContext));
            Device.Instance.RunGarbageCollection();
            _commandList.Open();
            try
            {
                _commandList.BeginMarker("URP.DLSS-NR");
                _dlss.Record(_commandList, new DlssNrDispatchDesc
                {
                    Color = _color,
                    Output = _output,
                    MotionVectors = _motion,
                    Depth = _depth,
                    InputWidth = Width,
                    InputHeight = Height,
                    OutputWidth = Width,
                    OutputHeight = Height,
                    MotionVectorScaleX = parameters.MotionScaleX,
                    MotionVectorScaleY = parameters.MotionScaleY,
                    Intensity = parameters.Intensity,
                    LocalToneStrength = parameters.LocalToneStrength,
                    LocalStructureStrength = parameters.LocalStructureStrength,
                    SkinStructureStrength = parameters.SkinStructureStrength,
                    DepthInverted = SystemInfo.usesReversedZBuffer,
                    Reset = parameters.Reset,
                    UseAutoMask = parameters.UseAutoMask,
                    UiCorrection = parameters.UiCorrection,
                    Upscaling = false,
                    Preset = parameters.Preset,
                    Style = parameters.Style,
                });
                _commandList.EndMarker();
                _commandList.Close();
                _commandList.SubmitAndForget(commandBuffer);
            }
            catch
            {
                // An exception while recording leaves a command list open. Replace
                // it so a transient managed failure cannot poison later frames.
                _commandList.Dispose();
                _commandList = new CommandList(8);
                throw;
            }
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            _commandList?.Dispose();
            _commandList = null;
            _dlss?.Dispose();
            _dlss = null;
            _output?.Dispose(); _output = null;
            _depth?.Dispose(); _depth = null;
            _motion?.Dispose(); _motion = null;
            _color?.Dispose(); _color = null;
            OutputHandle?.Release(); OutputHandle = null;
            DepthHandle?.Release(); DepthHandle = null;
            MotionHandle?.Release(); MotionHandle = null;
            ColorHandle?.Release(); ColorHandle = null;
            Destroy(OutputRt); OutputRt = null;
            Destroy(DepthRt); DepthRt = null;
            Destroy(MotionRt); MotionRt = null;
            Destroy(ColorRt); ColorRt = null;
        }

        private static RenderTexture CreateRenderTexture(string name, int width, int height,
            GraphicsFormat format)
        {
            var rt = new RenderTexture(new RenderTextureDescriptor(width, height)
            {
                graphicsFormat = format,
                depthStencilFormat = GraphicsFormat.None,
                msaaSamples = 1,
                volumeDepth = 1,
                dimension = UnityEngine.Rendering.TextureDimension.Tex2D,
                enableRandomWrite = true,
                sRGB = false,
                useMipMap = false,
            })
            {
                name = name,
                filterMode = FilterMode.Point,
                wrapMode = TextureWrapMode.Clamp,
                hideFlags = HideFlags.HideAndDontSave,
            };
            if (!rt.Create())
                throw new InvalidOperationException($"Could not create '{name}'.");
            return rt;
        }

        private static RhiTexture Wrap(Device device, RenderTexture renderTexture, Format format,
            ResourceStates initialState, string name)
        {
            return device.CreateTextureFromNativeResource(renderTexture.GetNativeTexturePtr(),
                new TextureDesc
                {
                    Width = (uint)renderTexture.width,
                    Height = (uint)renderTexture.height,
                    Format = format,
                    IsShaderResource = true,
                    IsUAV = true,
                    IsRenderTarget = true,
                    InitialState = initialState,
                    KeepInitialState = true,
                    DebugName = name,
                });
        }

        private static bool ProjectionChanged(in Matrix4x4 a, in Matrix4x4 b)
        {
            for (int i = 0; i < 16; ++i)
                if (Mathf.Abs(a[i] - b[i]) > 1e-4f)
                    return true;
            return false;
        }

        private static int SettingsHash(in DlssNrSettings settings)
        {
            unchecked
            {
                int hash = (int)settings.Preset;
                hash = hash * 397 ^ (int)settings.Style;
                hash = hash * 397 ^ settings.MotionVectorScale.x.GetHashCode();
                hash = hash * 397 ^ settings.MotionVectorScale.y.GetHashCode();
                hash = hash * 397 ^ (settings.UseAutoMask ? 1 : 0);
                hash = hash * 397 ^ (settings.UiCorrection ? 1 : 0);
                return hash;
            }
        }

        private static void Destroy(RenderTexture texture)
        {
            if (texture == null) return;
            texture.Release();
            if (Application.isPlaying)
                UnityEngine.Object.Destroy(texture);
            else
                UnityEngine.Object.DestroyImmediate(texture);
        }
    }
}
