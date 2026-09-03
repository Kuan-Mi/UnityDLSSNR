#if ENABLE_UPSCALER_FRAMEWORK
using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;
using RgTextureDesc = UnityEngine.Rendering.RenderGraphModule.TextureDesc;

namespace UnityRhi.Dlss.Urp
{
    /// <summary>
    /// URP IUpscaler that drives UnityRHI native NGX SuperSampling (DLSS / DLAA).
    /// Negotiates pre-upscale resolution, applies Halton jitter, and evaluates DLSS in post.
    /// XR keeps a separate NGX instance per eye (multipass or single-pass instanced).
    /// </summary>
    public sealed class UnityRhiDlssIUpscaler : AbstractUpscaler, IDisposable
    {
        public const string UpscalerName = "UnityRHI DLSS";

        private static readonly int InputColorId = UnityEngine.Shader.PropertyToID("_DlssInputColor");
        private static readonly int InputDepthId = UnityEngine.Shader.PropertyToID("_DlssInputDepth");
        private static readonly int InputMotionId = UnityEngine.Shader.PropertyToID("_DlssInputMotion");
        private static readonly int EyeSliceId = UnityEngine.Shader.PropertyToID("_DlssEyeSlice");
        private static readonly int CopySourceId = UnityEngine.Shader.PropertyToID("_DlssCopySource");
        private static readonly string ColorArrayKeyword = "_DLSS_COLOR_ARRAY";
        private static readonly string DepthArrayKeyword = "_DLSS_DEPTH_ARRAY";
        private static readonly string MotionArrayKeyword = "_DLSS_MOTION_ARRAY";
        private static readonly PropertyInfo RendererFrameDataProperty =
            typeof(ScriptableRenderer).GetProperty("frameData",
                BindingFlags.Instance | BindingFlags.NonPublic);

        private readonly UnityRhiDlssOptions _options;
        private readonly Dictionary<long, DlssCameraContext> _contexts = new();
        private static UnityRhiDlssIUpscaler _live;
        private Material _prepareMaterial;
        private Material _copyMaterial;
        private Vector2Int _inputResolution = new(1, 1);
        private Vector2Int _outputResolution = new(1, 1);
        private Vector2 _jitter;
        private UpscalerMode _modeHistory = UpscalerMode.QUALITY;
        private DlssPreset _presetHistory = DlssPreset.Default;
        private Vector2Int _outputResolutionPrevious;
        private bool _ready;
        private bool _warnedUnavailable;
        private bool _warnedFailure;
        private bool _loggedXr;
        private bool _disposed;

        private sealed class PreparePassData
        {
            public TextureHandle Color;
            public TextureHandle Depth;
            public TextureHandle Motion;
            public Material Material;
            public MaterialPropertyBlock Properties;
            public bool ColorArray;
            public bool DepthArray;
            public bool MotionArray;
            public float EyeSlice;
        }

        private sealed class DispatchPassData
        {
            public UnityRhiDlssIUpscaler Upscaler;
            public DlssCameraContext Context;
            public DlssCameraContext.DispatchParameters Parameters;
        }

        private sealed class CopyPassData
        {
            public TextureHandle Source;
            public Material Material;
            public MaterialPropertyBlock Properties;
        }

        public UnityRhiDlssIUpscaler(UnityRhiDlssOptions options)
        {
            _options = options != null
                ? options
                : (UnityRhiDlssOptions)ScriptableObject.CreateInstance(typeof(UnityRhiDlssOptions));
            if (string.IsNullOrEmpty(_options.UpscalerName))
                _options.UpscalerName = UpscalerName;

            _ready = RhiCore.IsD3D12Active && RhiCore.IsNgxDlssAvailable;
            if (!_ready && !_warnedUnavailable)
            {
                _warnedUnavailable = true;
                Debug.LogWarning(
                    $"[UnityRHI.DLSS] IUpscaler unavailable (D3D12={RhiCore.IsD3D12Active}, " +
                    $"NGX=0x{unchecked((uint)RhiCore.NgxDlssInitResult):X8}).");
            }

            UnityEngine.Shader shader = UnityEngine.Shader.Find("Hidden/UnityRHI/DLSS/PrepareInputs");
            if (shader != null)
            {
                _prepareMaterial = CoreUtils.CreateEngineMaterial(shader);
                _copyMaterial = CoreUtils.CreateEngineMaterial(shader);
            }
            else
            {
                _ready = false;
                Debug.LogError("[UnityRHI.DLSS] Missing Hidden/UnityRHI/DLSS/PrepareInputs shader.");
            }

            _modeHistory = ActiveQualityMode;
            _presetHistory = _options.preset;

            // URP constructs a new Upscaling/IUpscaler whenever the pipeline is
            // rebuilt and never disposes the previous instance. Release the
            // previous working set on the main thread before those wrappers
            // become unreachable and trip Resource.Finalize.
            if (_live != null && !ReferenceEquals(_live, this))
                _live.Dispose();
            _live = this;
            RhiDomainReload.RegisterOwner(this);
        }

        ~UnityRhiDlssIUpscaler() => Dispose(false);

        public override string GetName() => UpscalerName;

        public override bool IsTemporalUpscaler() => true;

        public override UpscalerOptions GetOptions() => _options;

        public override bool IsSupportedXR() => true;

        public override void CalculateJitter(int frameIndex, out Vector2 jitter, out bool allowScaling)
        {
            float upscaleRatio = (float)_outputResolution.x / Mathf.Max(1, _inputResolution.x);
            int numPhases = Mathf.Max(1, (int)(8.0f * upscaleRatio * upscaleRatio));
            int haltonIndex = (frameIndex % numPhases) + 1;
            float x = HaltonSequence.Get(haltonIndex, 2) - 0.5f;
            float y = HaltonSequence.Get(haltonIndex, 3) - 0.5f;
            jitter = new Vector2(x, y);
            allowScaling = false;
            _jitter = jitter;
        }

        public override void NegotiatePreUpscaleResolution(ref Vector2Int preUpscaleResolution,
            Vector2Int postUpscaleResolution)
        {
            _outputResolution = postUpscaleResolution;
            UpscalerMode mode = ActiveQualityMode;

            // URP only enables the temporal IUpscaler path for Game cameras
            // (imageScalingMode=Upscaling + forced TAA). Scene/Preview/Reflection still call
            // Negotiate but never run RecordRenderGraph upscale — if we lower their render
            // size here, the Scene view stays blurry at the reduced resolution.
            if (ShouldKeepNativeResolution())
            {
                preUpscaleResolution = postUpscaleResolution;
                _inputResolution = preUpscaleResolution;
                return;
            }

            if (mode == UpscalerMode.NATIVE || !(_options?.fixedResolutionMode ?? true))
            {
                if (mode == UpscalerMode.NATIVE)
                    preUpscaleResolution = postUpscaleResolution;
                _inputResolution = preUpscaleResolution;
                return;
            }

            if (!RhiCore.QueryDlssOptimalSettings(postUpscaleResolution.x, postUpscaleResolution.y,
                    mode, out int renderWidth, out int renderHeight))
            {
                float scale = UnityRhiDlssOptions.GetFixedRenderScale(mode);
                renderWidth = Mathf.Max(1, Mathf.RoundToInt(postUpscaleResolution.x * scale));
                renderHeight = Mathf.Max(1, Mathf.RoundToInt(postUpscaleResolution.y * scale));
            }

            preUpscaleResolution = new Vector2Int(renderWidth, renderHeight);
            _inputResolution = preUpscaleResolution;
        }

        /// <summary>
        /// True when the camera currently being initialized by URP is not a Game camera.
        /// Reads UniversalCameraData that CreateCameraData has already written into the
        /// active renderer's frameData before calling NegotiatePreUpscaleResolution.
        /// </summary>
        private static bool ShouldKeepNativeResolution()
        {
            try
            {
                UniversalRenderPipelineAsset asset = UniversalRenderPipeline.asset;
                if (asset == null || RendererFrameDataProperty == null)
                    return false;

                ScriptableRenderer renderer = asset.scriptableRenderer;
                if (renderer == null)
                    return false;

                var frameData = RendererFrameDataProperty.GetValue(renderer) as ContextContainer;
                if (frameData == null || !frameData.Contains<UniversalCameraData>())
                    return false;

                Camera camera = frameData.Get<UniversalCameraData>().camera;
                return camera != null && camera.cameraType != CameraType.Game;
            }
            catch
            {
                return false;
            }
        }

        public override void RecordRenderGraph(RenderGraph renderGraph, ContextContainer frameData)
        {
            if (_disposed || !_ready || _prepareMaterial == null || _copyMaterial == null)
                return;

            // Re-check NGX after plugin init (first frames may precede native init).
            if (!RhiCore.IsD3D12Active || !RhiCore.IsNgxDlssAvailable)
                return;

            UpscalingIO io = frameData.Get<UpscalingIO>();
            if (!io.cameraColor.IsValid() || !io.motionVectorColor.IsValid())
                return;

            // Prefer the sampled depth copy when URP provides it; fall back to camera depth.
            TextureHandle sourceDepth = io.cameraDepth;
            if (frameData.Contains<UniversalResourceData>())
            {
                UniversalResourceData resources = frameData.Get<UniversalResourceData>();
                if (resources.cameraDepthTexture.IsValid())
                    sourceDepth = resources.cameraDepthTexture;
            }
            if (!sourceDepth.IsValid())
                return;

            _inputResolution = io.preUpscaleResolution;
            _outputResolution = io.postUpscaleResolution;

            int renderWidth = io.preUpscaleResolution.x;
            int renderHeight = io.preUpscaleResolution.y;
            int outputWidth = io.postUpscaleResolution.x;
            int outputHeight = io.postUpscaleResolution.y;
            if (renderWidth <= 0 || renderHeight <= 0 || outputWidth <= 0 || outputHeight <= 0)
                return;

            RgTextureDesc colorDesc = io.cameraColor.GetDescriptor(renderGraph);
            RgTextureDesc depthDesc = sourceDepth.GetDescriptor(renderGraph);
            RgTextureDesc motionDesc = io.motionVectorColor.GetDescriptor(renderGraph);
            ResolveEyes(io, colorDesc, out int firstEye, out int eyeCount, out bool texArray);
            if (!_loggedXr && (texArray || firstEye != 0 ||
                (frameData.Contains<UniversalCameraData>() &&
                 frameData.Get<UniversalCameraData>().xr.enabled)))
            {
                _loggedXr = true;
                Debug.Log(texArray
                    ? $"[UnityRHI.DLSS] XR single-pass array: {eyeCount} eyes at " +
                      $"{renderWidth}x{renderHeight} -> {outputWidth}x{outputHeight}."
                    : $"[UnityRHI.DLSS] XR multipass eye {firstEye} at " +
                      $"{renderWidth}x{renderHeight} -> {outputWidth}x{outputHeight}.");
            }

            TextureHandle xrOutput = TextureHandle.nullHandle;
            if (texArray)
            {
                RgTextureDesc outputDesc = colorDesc;
                outputDesc.width = outputWidth;
                outputDesc.height = outputHeight;
                outputDesc.msaaSamples = MSAASamples.None;
                outputDesc.useMipMap = false;
                outputDesc.autoGenerateMips = false;
                outputDesc.enableRandomWrite = false;
                outputDesc.filterMode = FilterMode.Bilinear;
                outputDesc.clearBuffer = false;
                outputDesc.colorFormat = GraphicsFormat.R16G16B16A16_SFloat;
                outputDesc.name = "_UnityRhiDlssXrOutput";
                xrOutput = renderGraph.CreateTexture(outputDesc);
            }

            float motionVectorSign = io.motionVectorDirection ==
                UpscalingIO.MotionVectorDirection.PreviousFrameToCurrentFrame ? -1.0f : 1.0f;
            float motionVectorScaleX = io.motionVectorDomain == UpscalingIO.MotionVectorDomain.NDC
                ? io.motionVectorTextureSize.x : 1.0f;
            float motionVectorScaleY = io.motionVectorDomain == UpscalingIO.MotionVectorDomain.NDC
                ? io.motionVectorTextureSize.y : 1.0f;
            Vector2 userScale = _options != null ? _options.motionVectorScale : Vector2.one;
            bool forceReset = ShouldReset(io);

            TextureHandle lastOutput = TextureHandle.nullHandle;
            for (int i = 0; i < eyeCount; ++i)
            {
                int eye = firstEye + i;
                if (!TryRecordEye(renderGraph, io, sourceDepth, renderWidth, renderHeight,
                        outputWidth, outputHeight, eye, texArray, IsTexArray(colorDesc),
                        IsTexArray(depthDesc), IsTexArray(motionDesc), xrOutput,
                        motionVectorSign * motionVectorScaleX * userScale.x,
                        motionVectorSign * motionVectorScaleY * userScale.y,
                        forceReset || io.resetHistory, out lastOutput))
                    return;
            }

            io.cameraColor = texArray ? xrOutput : lastOutput;
            _outputResolutionPrevious = io.postUpscaleResolution;
            _modeHistory = ActiveQualityMode;
            _presetHistory = _options.preset;
        }

        private bool TryRecordEye(RenderGraph renderGraph, UpscalingIO io,
            TextureHandle sourceDepth, int renderWidth, int renderHeight,
            int outputWidth, int outputHeight, int eye, bool texArray,
            bool colorArray, bool depthArray, bool motionArray, TextureHandle xrOutput,
            float motionScaleX, float motionScaleY, bool forceReset, out TextureHandle output)
        {
            output = TextureHandle.nullHandle;
            DlssCameraContext context;
            try
            {
                context = GetOrCreateContext(io.cameraInstanceID, eye, renderWidth, renderHeight,
                    outputWidth, outputHeight);
            }
            catch (Exception exception)
            {
                if (!_warnedFailure)
                {
                    _warnedFailure = true;
                    Debug.LogError($"[UnityRHI.DLSS] Resource init failed. {exception}");
                }
                return false;
            }

            TextureHandle color = renderGraph.ImportTexture(context.ColorHandle);
            TextureHandle depth = renderGraph.ImportTexture(context.DepthHandle);
            TextureHandle motion = renderGraph.ImportTexture(context.MotionHandle);
            output = renderGraph.ImportTexture(context.OutputHandle);

            using (IRasterRenderGraphBuilder builder =
                renderGraph.AddRasterRenderPass<PreparePassData>(
                    texArray ? $"UnityRHI DLSS Prepare Eye {eye}" : "UnityRHI DLSS Prepare",
                    out PreparePassData prepareData))
            {
                prepareData.Color = io.cameraColor;
                prepareData.Depth = sourceDepth;
                prepareData.Motion = io.motionVectorColor;
                prepareData.Material = _prepareMaterial;
                prepareData.Properties = new MaterialPropertyBlock();
                prepareData.ColorArray = colorArray;
                prepareData.DepthArray = depthArray;
                prepareData.MotionArray = motionArray;
                prepareData.EyeSlice = eye;
                builder.UseTexture(io.cameraColor, AccessFlags.Read);
                builder.UseTexture(sourceDepth, AccessFlags.Read);
                builder.UseTexture(io.motionVectorColor, AccessFlags.Read);
                builder.SetRenderAttachment(color, 0, AccessFlags.WriteAll);
                builder.SetRenderAttachment(motion, 1, AccessFlags.WriteAll);
                builder.SetRenderAttachment(depth, 2, AccessFlags.WriteAll);
                builder.AllowPassCulling(false);
                builder.SetRenderFunc(static (PreparePassData data, RasterGraphContext rgContext) =>
                {
                    SetKeyword(data.Material, ColorArrayKeyword, data.ColorArray);
                    SetKeyword(data.Material, DepthArrayKeyword, data.DepthArray);
                    SetKeyword(data.Material, MotionArrayKeyword, data.MotionArray);
                    // DrawProcedural keeps a live Material reference. Single-pass records
                    // one prepare per eye against the same material, so SetTexture/SetFloat
                    // would both execute as the last eye (right). MPB is copied per draw.
                    data.Properties.Clear();
                    data.Properties.SetTexture(InputColorId, data.Color);
                    data.Properties.SetTexture(InputDepthId, data.Depth);
                    data.Properties.SetTexture(InputMotionId, data.Motion);
                    data.Properties.SetFloat(EyeSliceId, data.EyeSlice);
                    rgContext.cmd.DrawProcedural(Matrix4x4.identity, data.Material, 0,
                        MeshTopology.Triangles, 3, 1, data.Properties);
                });
            }

            using (IUnsafeRenderGraphBuilder builder =
                renderGraph.AddUnsafePass<DispatchPassData>(
                    texArray ? $"UnityRHI DLSS Eye {eye}" : "UnityRHI DLSS",
                    out DispatchPassData passData, new ProfilingSampler("UnityRHI DLSS")))
            {
                passData.Upscaler = this;
                passData.Context = context;
                passData.Parameters = context.BeginFrame(io.frameIndex, _jitter,
                    motionScaleX, motionScaleY, ActiveQualityMode, _options.preset, forceReset);
                builder.UseTexture(color, AccessFlags.Read);
                builder.UseTexture(depth, AccessFlags.Read);
                builder.UseTexture(motion, AccessFlags.Read);
                builder.UseTexture(output, AccessFlags.WriteAll);
                builder.AllowPassCulling(false);
                builder.AllowGlobalStateModification(true);
                builder.SetRenderFunc(static (DispatchPassData data, UnsafeGraphContext unsafeContext) =>
                {
                    try
                    {
                        CommandBuffer commandBuffer =
                            CommandBufferHelpers.GetNativeCommandBuffer(unsafeContext.cmd);
                        data.Context.Record(commandBuffer, data.Parameters);
                    }
                    catch (Exception exception)
                    {
                        if (!data.Upscaler._warnedFailure)
                        {
                            data.Upscaler._warnedFailure = true;
                            Debug.LogError($"[UnityRHI.DLSS] Dispatch failed. {exception}");
                        }
                    }
                });
            }

            if (texArray)
                RecordCopyToSlice(renderGraph, output, xrOutput, eye);
            return true;
        }

        private void RecordCopyToSlice(RenderGraph renderGraph, TextureHandle source,
            TextureHandle destination, int eye)
        {
            using (IRasterRenderGraphBuilder builder =
                renderGraph.AddRasterRenderPass<CopyPassData>(
                    $"UnityRHI DLSS Copy Eye {eye}", out CopyPassData passData))
            {
                passData.Source = source;
                passData.Material = _copyMaterial;
                passData.Properties = new MaterialPropertyBlock();
                builder.UseTexture(source, AccessFlags.Read);
                builder.SetRenderAttachment(destination, 0, AccessFlags.ReadWrite, 0, eye);
                builder.AllowPassCulling(false);
                builder.SetRenderFunc(static (CopyPassData data, RasterGraphContext rgContext) =>
                {
                    data.Properties.Clear();
                    data.Properties.SetTexture(CopySourceId, data.Source);
                    rgContext.cmd.DrawProcedural(Matrix4x4.identity, data.Material, 2,
                        MeshTopology.Triangles, 3, 1, data.Properties);
                });
            }
        }

        private static bool IsTexArray(in RgTextureDesc desc) =>
            desc.dimension == UnityEngine.Rendering.TextureDimension.Tex2DArray && desc.slices > 1;

        private static void ResolveEyes(UpscalingIO io, in RgTextureDesc colorDesc,
            out int firstEye, out int eyeCount, out bool texArray)
        {
            firstEye = io.eyeIndex;
            if (io.enableTexArray && IsTexArray(colorDesc))
            {
                eyeCount = Mathf.Max(1, io.numActiveViews);
                texArray = true;
                return;
            }

            eyeCount = 1;
            texArray = false;
        }

        private static void SetKeyword(Material material, string keyword, bool enabled)
        {
            if (enabled)
                material.EnableKeyword(keyword);
            else
                material.DisableKeyword(keyword);
        }

        private UpscalerMode ActiveQualityMode =>
            UnityRhiDlssOptions.SanitizeQualityMode(
                _options != null ? _options.qualityMode : UpscalerMode.QUALITY);

        private bool ShouldReset(UpscalingIO io) =>
            _modeHistory != ActiveQualityMode ||
            _presetHistory != _options.preset ||
            _outputResolutionPrevious != io.postUpscaleResolution;

        private static long MakeContextKey(int cameraInstanceId, int eye) =>
            cameraInstanceId + eye * 100000L;

        private DlssCameraContext GetOrCreateContext(int cameraInstanceId, int eye,
            int renderWidth, int renderHeight, int outputWidth, int outputHeight)
        {
            long key = MakeContextKey(cameraInstanceId, eye);
            if (_contexts.TryGetValue(key, out DlssCameraContext context))
            {
                if (context.Matches(renderWidth, renderHeight, outputWidth, outputHeight))
                    return context;
                context.Dispose();
                _contexts.Remove(key);
            }

            context = new DlssCameraContext(renderWidth, renderHeight, outputWidth, outputHeight,
                $"Cam{cameraInstanceId}_Eye{eye}");
            _contexts.Add(key, context);
            return context;
        }

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        private void Dispose(bool disposing)
        {
            if (_disposed)
                return;
            _disposed = true;
            if (ReferenceEquals(_live, this))
                _live = null;
            RhiDomainReload.UnregisterOwner(this);

            foreach (DlssCameraContext context in _contexts.Values)
                context.Release(disposing);
            _contexts.Clear();

            if (!disposing)
                return;
            if (_prepareMaterial != null)
            {
                CoreUtils.Destroy(_prepareMaterial);
                _prepareMaterial = null;
            }
            if (_copyMaterial != null)
            {
                CoreUtils.Destroy(_copyMaterial);
                _copyMaterial = null;
            }
        }
    }
}
#endif
