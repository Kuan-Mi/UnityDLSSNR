using System;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using RhiTexture = UnityRhi.Texture;

namespace UnityRhi.Dlss.Urp
{
    /// <summary>Persistent Unity/UnityRHI resources and NGX history for one camera eye.</summary>
    internal sealed class DlssCameraContext : IDisposable
    {
        internal readonly struct DispatchParameters
        {
            internal readonly bool Reset;
            internal readonly Vector2 JitterPixels;
            internal readonly float MotionScaleX;
            internal readonly float MotionScaleY;
            internal readonly UpscalerMode Mode;
            internal readonly DlssPreset Preset;

            internal DispatchParameters(bool reset, Vector2 jitterPixels,
                float motionScaleX, float motionScaleY, UpscalerMode mode, DlssPreset preset)
            {
                Reset = reset;
                JitterPixels = jitterPixels;
                MotionScaleX = motionScaleX;
                MotionScaleY = motionScaleY;
                Mode = mode;
                Preset = preset;
            }
        }

        internal int RenderWidth { get; }
        internal int RenderHeight { get; }
        internal int OutputWidth { get; }
        internal int OutputHeight { get; }
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
        private DlssContext _dlss;
        private CommandList _commandList;
        private int _lastFrame = int.MinValue;
        private UpscalerMode _lastMode;
        private DlssPreset _lastPreset;
        private bool _hasHistory;
        private bool _disposed;

        internal DlssCameraContext(int renderWidth, int renderHeight, int outputWidth, int outputHeight,
            string cameraName)
        {
            if (renderWidth <= 0) throw new ArgumentOutOfRangeException(nameof(renderWidth));
            if (renderHeight <= 0) throw new ArgumentOutOfRangeException(nameof(renderHeight));
            if (outputWidth <= 0) throw new ArgumentOutOfRangeException(nameof(outputWidth));
            if (outputHeight <= 0) throw new ArgumentOutOfRangeException(nameof(outputHeight));
            RenderWidth = renderWidth;
            RenderHeight = renderHeight;
            OutputWidth = outputWidth;
            OutputHeight = outputHeight;

            try
            {
                ColorRt = CreateRenderTexture($"DLSS {cameraName} Color", renderWidth, renderHeight,
                    GraphicsFormat.R16G16B16A16_SFloat, enableRandomWrite: false);
                MotionRt = CreateRenderTexture($"DLSS {cameraName} Motion", renderWidth, renderHeight,
                    GraphicsFormat.R16G16_SFloat, enableRandomWrite: false);
                DepthRt = CreateRenderTexture($"DLSS {cameraName} Depth", renderWidth, renderHeight,
                    GraphicsFormat.R32_SFloat, enableRandomWrite: false);
                OutputRt = CreateRenderTexture($"DLSS {cameraName} Output", outputWidth, outputHeight,
                    GraphicsFormat.R16G16B16A16_SFloat, enableRandomWrite: true);

                ColorHandle = RTHandles.Alloc(ColorRt);
                MotionHandle = RTHandles.Alloc(MotionRt);
                DepthHandle = RTHandles.Alloc(DepthRt);
                OutputHandle = RTHandles.Alloc(OutputRt);

                Device device = Device.Instance;
                _color = Wrap(device, ColorRt, Format.RGBA16_FLOAT,
                    ResourceStates.RenderTarget, $"DLSS {cameraName} Color");
                _motion = Wrap(device, MotionRt, Format.RG16_FLOAT,
                    ResourceStates.RenderTarget, $"DLSS {cameraName} Motion");
                _depth = Wrap(device, DepthRt, Format.R32_FLOAT,
                    ResourceStates.RenderTarget, $"DLSS {cameraName} Depth");
                _output = Wrap(device, OutputRt, Format.RGBA16_FLOAT,
                    ResourceStates.UnorderedAccess, $"DLSS {cameraName} Output");
                _dlss = new DlssContext();
                _commandList = new CommandList(8);
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        internal bool Matches(int renderWidth, int renderHeight, int outputWidth, int outputHeight) =>
            RenderWidth == renderWidth && RenderHeight == renderHeight &&
            OutputWidth == outputWidth && OutputHeight == outputHeight;

        internal DispatchParameters BeginFrame(int frameIndex, Vector2 jitterPixels,
            float motionScaleX, float motionScaleY, UpscalerMode mode, DlssPreset preset,
            bool forceReset)
        {
            bool reset = forceReset || !_hasHistory || frameIndex != _lastFrame + 1 ||
                mode != _lastMode || preset != _lastPreset;
            _lastFrame = frameIndex;
            _lastMode = mode;
            _lastPreset = preset;
            _hasHistory = true;
            return new DispatchParameters(reset, jitterPixels, motionScaleX, motionScaleY, mode, preset);
        }

        internal void ResetHistory() => _hasHistory = false;

        internal void Record(CommandBuffer commandBuffer, in DispatchParameters parameters)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(DlssCameraContext));
            Device.Instance.RunGarbageCollection();
            _commandList.Open();
            try
            {
                _commandList.BeginMarker("URP.DLSS");
                _dlss.Record(_commandList, new DlssDispatchDesc
                {
                    Input = _color,
                    Output = _output,
                    MotionVectors = _motion,
                    Depth = _depth,
                    CameraJitterPixels = parameters.JitterPixels,
                    RenderWidth = RenderWidth,
                    RenderHeight = RenderHeight,
                    OutputWidth = OutputWidth,
                    OutputHeight = OutputHeight,
                    MotionVectorScaleX = parameters.MotionScaleX,
                    MotionVectorScaleY = parameters.MotionScaleY,
                    Mode = parameters.Mode,
                    Preset = parameters.Preset,
                    Reset = parameters.Reset,
                    DepthInverted = SystemInfo.usesReversedZBuffer,
                });
                _commandList.EndMarker();
                _commandList.Close();
                _commandList.SubmitAndForget(commandBuffer);
            }
            catch
            {
                _commandList.Dispose();
                _commandList = new CommandList(8);
                throw;
            }
        }

        public void Dispose() => Release(true);

        ~DlssCameraContext() => Release(false);

        internal void Release(bool disposing)
        {
            if (_disposed) return;
            _disposed = true;
            GC.SuppressFinalize(this);
            _commandList?.Dispose();
            _commandList = null;
            _dlss?.Dispose();
            _dlss = null;
            _output?.Dispose(); _output = null;
            _depth?.Dispose(); _depth = null;
            _motion?.Dispose(); _motion = null;
            _color?.Dispose(); _color = null;
            if (!disposing)
                return;
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
            GraphicsFormat format, bool enableRandomWrite)
        {
            var rt = new RenderTexture(new RenderTextureDescriptor(width, height)
            {
                graphicsFormat = format,
                depthStencilFormat = GraphicsFormat.None,
                msaaSamples = 1,
                volumeDepth = 1,
                dimension = UnityEngine.Rendering.TextureDimension.Tex2D,
                enableRandomWrite = enableRandomWrite,
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
