using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;

namespace UnityRhi.Dlss.Urp
{
    /// <summary>
    /// SDR DLSS Neural Rendering for Unity 6.3 URP.
    /// Default injection is before post/upscale so the Game camera order is
    /// raster → NR → remaining post. XR uses per-eye history at native resolution
    /// (multipass or single-pass instanced texture arrays).
    /// </summary>
    [DisallowMultipleRendererFeature("DLSS Neural Rendering")]
    public sealed class DlssNrRenderFeature : ScriptableRendererFeature
    {
        [Tooltip("Default Before Rendering Post Processing runs NR at render resolution " +
                 "(NR → remaining post). After Rendering Post Processing is the " +
                 "optional post → NR stack.")]
        public RenderPassEvent renderPassEvent = RenderPassEvent.BeforeRenderingPostProcessing;

        [Tooltip("Allow the effect in the Scene view when its Volume is active.")]
        public bool runInSceneView;

        [Tooltip("Only process the camera that resolves the final camera-stack target.")]
        public bool finalCameraInStackOnly = true;

        [SerializeField, Tooltip("Input preparation shader. Automatically resolved from the package when possible.")]
        private UnityEngine.Shader prepareInputsShader;

        private Material _prepareMaterial;
        private Material _debugMaterial;
        private Material _copyMaterial;
        private DlssNrPass _pass;
        private readonly Dictionary<long, DlssNrCameraContext> _contexts =
            new Dictionary<long, DlssNrCameraContext>();
        private readonly Dictionary<long, Camera> _contextCameras =
            new Dictionary<long, Camera>();
        private readonly List<long> _deadKeys = new List<long>();
        private bool _warnedUnavailable;
        private bool _warnedHdr;
        private bool _warnedFailure;
        private bool _loggedSrStack;
        private bool _loggedNrThenSr;
        private bool _loggedXr;

        public override void Create()
        {
            if (prepareInputsShader == null)
                prepareInputsShader = UnityEngine.Shader.Find("Hidden/UnityRHI/DLSS-NR/PrepareInputs");
            if (_prepareMaterial != null)
                CoreUtils.Destroy(_prepareMaterial);
            if (_debugMaterial != null)
                CoreUtils.Destroy(_debugMaterial);
            if (_copyMaterial != null)
                CoreUtils.Destroy(_copyMaterial);
            _prepareMaterial = prepareInputsShader != null
                ? CoreUtils.CreateEngineMaterial(prepareInputsShader) : null;
            // RenderGraph records prepare, debug and copy draws separately.
            // Independent materials keep later pass bindings from mutating
            // earlier recorded draws.
            _debugMaterial = prepareInputsShader != null
                ? CoreUtils.CreateEngineMaterial(prepareInputsShader) : null;
            _copyMaterial = prepareInputsShader != null
                ? CoreUtils.CreateEngineMaterial(prepareInputsShader) : null;
            _pass = new DlssNrPass(this) { renderPassEvent = renderPassEvent };
#if UNITY_EDITOR
            RhiDomainReload.RegisterOwner(this);
#endif
        }

        public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData renderingData)
        {
            if (_prepareMaterial == null || _debugMaterial == null || _copyMaterial == null)
                return;
            DlssNrVolume volume = VolumeManager.instance.stack?.GetComponent<DlssNrVolume>();
            if (volume == null || !volume.IsActive())
                return;
            if (!RhiCore.IsD3D12Active || !RhiCore.IsDlssNrAvailable)
            {
                if (!_warnedUnavailable)
                {
                    _warnedUnavailable = true;
                    Debug.LogWarning($"[UnityRHI.DLSS-NR] Pass disabled: D3D12/NR runtime unavailable " +
                        $"(init=0x{unchecked((uint)RhiCore.DlssNrInitResult):X8}).");
                }
                return;
            }

            CameraData cameraData = renderingData.cameraData;
            Camera camera = cameraData.camera;
            if (camera == null || camera.cameraType == CameraType.Preview ||
                camera.cameraType == CameraType.Reflection)
                return;
            if (!runInSceneView && camera.cameraType == CameraType.SceneView)
                return;
            if (finalCameraInStackOnly && !cameraData.resolveFinalTarget)
                return;

            _pass.renderPassEvent = renderPassEvent;
            _pass.ConfigureInput(ScriptableRenderPassInput.Depth | ScriptableRenderPassInput.Motion);
            _pass.requiresIntermediateTexture = true;
            renderer.EnqueuePass(_pass);
        }

        /// <summary>Reset temporal history for all live camera contexts.</summary>
        public void ResetHistory()
        {
            foreach (DlssNrCameraContext context in _contexts.Values)
                context.ResetHistory();
        }

        protected override void Dispose(bool disposing)
        {
            foreach (DlssNrCameraContext context in _contexts.Values)
                context.Dispose();
            _contexts.Clear();
            _contextCameras.Clear();
            _deadKeys.Clear();
            if (_prepareMaterial != null)
                CoreUtils.Destroy(_prepareMaterial);
            if (_debugMaterial != null)
                CoreUtils.Destroy(_debugMaterial);
            if (_copyMaterial != null)
                CoreUtils.Destroy(_copyMaterial);
            _prepareMaterial = null;
            _debugMaterial = null;
            _copyMaterial = null;
            _pass = null;
            base.Dispose(disposing);
        }

        private static long MakeContextKey(Camera camera, int eye) =>
            camera.GetInstanceID() + eye * 100000L;

        private DlssNrCameraContext GetContext(Camera camera, int eye, int width, int height,
            bool xr)
        {
            PruneDeadCameras();
            long key = MakeContextKey(camera, eye);
            if (_contexts.TryGetValue(key, out DlssNrCameraContext context))
            {
                if (context.Width == width && context.Height == height)
                    return context;
                context.Dispose();
                _contexts.Remove(key);
                _contextCameras.Remove(key);
            }

            string name = xr ? $"{camera.name}_Eye{eye}" : camera.name;
            context = new DlssNrCameraContext(width, height, name);
            _contexts.Add(key, context);
            _contextCameras[key] = camera;
            return context;
        }

        private void PruneDeadCameras()
        {
            _deadKeys.Clear();
            foreach (KeyValuePair<long, Camera> pair in _contextCameras)
                if (pair.Value == null)
                    _deadKeys.Add(pair.Key);
            foreach (long key in _deadKeys)
            {
                if (_contexts.TryGetValue(key, out DlssNrCameraContext context))
                    context.Dispose();
                _contexts.Remove(key);
                _contextCameras.Remove(key);
            }
            _deadKeys.Clear();
        }

        private sealed class DlssNrPass : ScriptableRenderPass
        {
            private static readonly int InputColorId = UnityEngine.Shader.PropertyToID("_DlssNrInputColor");
            private static readonly int InputDepthId = UnityEngine.Shader.PropertyToID("_DlssNrInputDepth");
            private static readonly int InputMotionId = UnityEngine.Shader.PropertyToID("_DlssNrInputMotion");
            private static readonly int EyeSliceId = UnityEngine.Shader.PropertyToID("_DlssNrEyeSlice");
            private static readonly int CopySourceId = UnityEngine.Shader.PropertyToID("_DlssNrCopySource");
            private static readonly int DebugModeId = UnityEngine.Shader.PropertyToID("_DlssNrDebugMode");
            private static readonly int DebugMotionScaleXId = UnityEngine.Shader.PropertyToID("_DlssNrDebugMotionScaleX");
            private static readonly int DebugMotionScaleYId = UnityEngine.Shader.PropertyToID("_DlssNrDebugMotionScaleY");
            private static readonly int DebugMotionRangeId = UnityEngine.Shader.PropertyToID("_DlssNrDebugMotionRange");
            private static readonly int DebugDepthRangeId = UnityEngine.Shader.PropertyToID("_DlssNrDebugDepthRange");
            private static readonly string ColorArrayKeyword = "_DLSSNR_COLOR_ARRAY";
            private static readonly string DepthArrayKeyword = "_DLSSNR_DEPTH_ARRAY";
            private static readonly string MotionArrayKeyword = "_DLSSNR_MOTION_ARRAY";
            private readonly DlssNrRenderFeature _feature;

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
                public DlssNrRenderFeature Feature;
                public DlssNrCameraContext Context;
                public DlssNrCameraContext.DispatchParameters Parameters;
            }

            private sealed class DebugPassData
            {
                public TextureHandle Depth;
                public TextureHandle Motion;
                public Material Material;
                public MaterialPropertyBlock Properties;
                public int Mode;
                public float MotionScaleX;
                public float MotionScaleY;
                public float MotionRange;
                public float DepthRange;
            }

            private sealed class CopyPassData
            {
                public TextureHandle Source;
                public Material Material;
                public MaterialPropertyBlock Properties;
            }

            internal DlssNrPass(DlssNrRenderFeature feature)
            {
                _feature = feature;
                profilingSampler = new ProfilingSampler("DLSS Neural Rendering");
            }

            public override void RecordRenderGraph(RenderGraph renderGraph, ContextContainer frameData)
            {
                UniversalResourceData resources = frameData.Get<UniversalResourceData>();
                UniversalCameraData cameraData = frameData.Get<UniversalCameraData>();
                Camera camera = cameraData.camera;
                if (camera == null || resources.isActiveTargetBackBuffer)
                    return;
                if (_feature.finalCameraInStackOnly && !cameraData.resolveFinalTarget)
                    return;
                DlssNrVolume volume = VolumeManager.instance.stack?.GetComponent<DlssNrVolume>();
                if (volume == null || !volume.IsActive())
                    return;
                var settings = new DlssNrSettings(volume);
                if (cameraData.isHDROutputActive)
                {
                    if (!_feature._warnedHdr)
                    {
                        _feature._warnedHdr = true;
                        Debug.LogWarning("[UnityRHI.DLSS-NR] HDR Output is not supported on this SDR path; bypassing.");
                    }
                    return;
                }

                TextureHandle sourceColor = resources.activeColorTexture;
                TextureHandle sourceDepth = resources.cameraDepthTexture;
                TextureHandle sourceMotion = resources.motionVectorColor;
                if (!sourceColor.IsValid() || !sourceDepth.IsValid() || !sourceMotion.IsValid())
                    return;

                UnityEngine.Rendering.RenderGraphModule.TextureDesc sourceDesc =
                    renderGraph.GetTextureDesc(sourceColor);
                int width = sourceDesc.width;
                int height = sourceDesc.height;
                if (width <= 0 || height <= 0)
                    return;

                UnityEngine.Rendering.RenderGraphModule.TextureDesc depthDesc =
                    renderGraph.GetTextureDesc(sourceDepth);
                UnityEngine.Rendering.RenderGraphModule.TextureDesc motionDesc =
                    renderGraph.GetTextureDesc(sourceMotion);
                bool auxLowerRes = depthDesc.width < width || depthDesc.height < height ||
                    motionDesc.width < width || motionDesc.height < height;
                if (auxLowerRes && !_feature._loggedSrStack)
                {
                    _feature._loggedSrStack = true;
                    Debug.Log(
                        $"[UnityRHI.DLSS-NR] SR→NR stack: color {width}x{height}, " +
                        $"depth {depthDesc.width}x{depthDesc.height}, " +
                        $"motion {motionDesc.width}x{motionDesc.height}. " +
                        "Upsampling depth/MV into NR inputs.");
                }
                else if (!auxLowerRes && !_feature._loggedNrThenSr)
                {
                    _feature._loggedNrThenSr = true;
                    Debug.Log(
                        $"[UnityRHI.DLSS-NR] NR at {width}x{height} " +
                        "(native / render resolution).");
                }

                ResolveEyes(cameraData, sourceDesc, out int firstEye, out int eyeCount,
                    out bool texArray);
                if (texArray && !_feature._loggedXr)
                {
                    _feature._loggedXr = true;
                    Debug.Log(
                        $"[UnityRHI.DLSS-NR] XR single-pass array: {eyeCount} eyes at {width}x{height}.");
                }
                else if (cameraData.xr.enabled && !texArray && !_feature._loggedXr)
                {
                    _feature._loggedXr = true;
                    Debug.Log(
                        $"[UnityRHI.DLSS-NR] XR multipass eye {firstEye} at {width}x{height}.");
                }

                TextureHandle destColor = resources.cameraColor.IsValid()
                    ? resources.cameraColor
                    : sourceColor;
                bool colorArray = IsTexArray(sourceDesc);
                bool depthArray = IsTexArray(depthDesc);
                bool motionArray = IsTexArray(motionDesc);

                TextureHandle lastOutput = TextureHandle.nullHandle;
                for (int i = 0; i < eyeCount; ++i)
                {
                    int eye = firstEye + i;
                    if (!TryRecordEye(renderGraph, cameraData, camera, settings,
                            sourceColor, sourceDepth, sourceMotion, destColor, width, height,
                            eye, texArray, colorArray, depthArray, motionArray, out lastOutput))
                        return;
                }

                if (!texArray && lastOutput.IsValid())
                    resources.cameraColor = lastOutput;
            }

            private static bool IsTexArray(
                in UnityEngine.Rendering.RenderGraphModule.TextureDesc desc) =>
                desc.dimension == UnityEngine.Rendering.TextureDimension.Tex2DArray &&
                desc.slices > 1;

            private static void ResolveEyes(UniversalCameraData cameraData,
                in UnityEngine.Rendering.RenderGraphModule.TextureDesc sourceDesc,
                out int firstEye, out int eyeCount, out bool texArray)
            {
                // NativeNrd keys history by cameraId + multipassId * 100000. Single-pass
                // instanced still uses that key scheme, but NGX only accepts 2D resources,
                // so we extract each array slice into a per-eye 2D context.
                texArray = false;
                if (cameraData.xr.enabled && cameraData.xr.singlePassEnabled &&
                    IsTexArray(sourceDesc))
                {
                    firstEye = 0;
                    eyeCount = Mathf.Max(1, cameraData.xr.viewCount);
                    texArray = true;
                    return;
                }

                firstEye = cameraData.xr.enabled ? cameraData.xr.multipassId : 0;
                eyeCount = 1;
            }

            private bool TryRecordEye(RenderGraph renderGraph,
                UniversalCameraData cameraData, Camera camera, in DlssNrSettings settings,
                TextureHandle sourceColor, TextureHandle sourceDepth, TextureHandle sourceMotion,
                TextureHandle destColor, int width, int height, int eye, bool texArray,
                bool colorArray, bool depthArray, bool motionArray, out TextureHandle output)
            {
                output = TextureHandle.nullHandle;
                DlssNrCameraContext context;
                try
                {
                    context = _feature.GetContext(camera, eye, width, height,
                        cameraData.xr.enabled);
                }
                catch (Exception exception)
                {
                    if (!_feature._warnedFailure)
                    {
                        _feature._warnedFailure = true;
                        Debug.LogError($"[UnityRHI.DLSS-NR] Resource initialization failed; bypassing. {exception}");
                    }
                    return false;
                }

                TextureHandle color = renderGraph.ImportTexture(context.ColorHandle);
                TextureHandle depth = renderGraph.ImportTexture(context.DepthHandle);
                TextureHandle motion = renderGraph.ImportTexture(context.MotionHandle);
                output = renderGraph.ImportTexture(context.OutputHandle);

                using (IRasterRenderGraphBuilder builder =
                    renderGraph.AddRasterRenderPass<PreparePassData>(
                        texArray ? $"DLSS-NR Prepare Inputs Eye {eye}" : "DLSS-NR Prepare Inputs",
                        out PreparePassData passData))
                {
                    passData.Color = sourceColor;
                    passData.Depth = sourceDepth;
                    passData.Motion = sourceMotion;
                    passData.Material = _feature._prepareMaterial;
                    passData.Properties = new MaterialPropertyBlock();
                    passData.ColorArray = colorArray;
                    passData.DepthArray = depthArray;
                    passData.MotionArray = motionArray;
                    passData.EyeSlice = eye;
                    builder.UseTexture(sourceColor, AccessFlags.Read);
                    builder.UseTexture(sourceDepth, AccessFlags.Read);
                    builder.UseTexture(sourceMotion, AccessFlags.Read);
                    builder.SetRenderAttachment(color, 0, AccessFlags.WriteAll);
                    builder.SetRenderAttachment(motion, 1, AccessFlags.WriteAll);
                    builder.SetRenderAttachment(depth, 2, AccessFlags.WriteAll);
                    builder.SetRenderAttachment(output, 3, AccessFlags.WriteAll);
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

                GetEyePose(cameraData, camera, eye,
                    out Vector3 position, out Quaternion rotation, out Matrix4x4 projection);

                if (settings.DebugMode != DlssNrDebugMode.Off)
                {
                    using (IRasterRenderGraphBuilder builder =
                        renderGraph.AddRasterRenderPass<DebugPassData>("DLSS-NR Debug Inputs",
                            out DebugPassData passData, profilingSampler))
                    {
                        passData.Depth = depth;
                        passData.Motion = motion;
                        passData.Material = _feature._debugMaterial;
                        passData.Properties = new MaterialPropertyBlock();
                        passData.Mode = (int)settings.DebugMode;
                        passData.MotionScaleX = -width * settings.MotionVectorScale.x;
                        passData.MotionScaleY = -height * settings.MotionVectorScale.y;
                        passData.MotionRange = settings.DebugMotionRange;
                        passData.DepthRange = settings.DebugDepthRange;
                        builder.UseTexture(depth, AccessFlags.Read);
                        builder.UseTexture(motion, AccessFlags.Read);
                        builder.SetRenderAttachment(output, 0, AccessFlags.WriteAll);
                        builder.AllowPassCulling(false);
                        builder.SetRenderFunc(static (DebugPassData data, RasterGraphContext rgContext) =>
                        {
                            data.Properties.Clear();
                            data.Properties.SetTexture(InputDepthId, data.Depth);
                            data.Properties.SetTexture(InputMotionId, data.Motion);
                            data.Properties.SetInt(DebugModeId, data.Mode);
                            data.Properties.SetFloat(DebugMotionScaleXId, data.MotionScaleX);
                            data.Properties.SetFloat(DebugMotionScaleYId, data.MotionScaleY);
                            data.Properties.SetFloat(DebugMotionRangeId, data.MotionRange);
                            data.Properties.SetFloat(DebugDepthRangeId, data.DepthRange);
                            rgContext.cmd.DrawProcedural(Matrix4x4.identity, data.Material, 1,
                                MeshTopology.Triangles, 3, 1, data.Properties);
                        });
                    }

                    if (texArray)
                        RecordCopyToSlice(renderGraph, output, destColor, eye);
                    return true;
                }

                using (IUnsafeRenderGraphBuilder builder =
                    renderGraph.AddUnsafePass<DispatchPassData>(
                        texArray ? $"DLSS Neural Rendering Eye {eye}" : "DLSS Neural Rendering",
                        out DispatchPassData passData, profilingSampler))
                {
                    passData.Feature = _feature;
                    passData.Context = context;
                    passData.Parameters = context.BeginFrame(Time.frameCount, position,
                        rotation, projection, settings);
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
                            if (!data.Feature._warnedFailure)
                            {
                                data.Feature._warnedFailure = true;
                                Debug.LogError($"[UnityRHI.DLSS-NR] Dispatch failed. {exception}");
                            }
                        }
                    });
                }

                if (texArray)
                    RecordCopyToSlice(renderGraph, output, destColor, eye);
                return true;
            }

            private void RecordCopyToSlice(RenderGraph renderGraph, TextureHandle source,
                TextureHandle destination, int eye)
            {
                using (IRasterRenderGraphBuilder builder =
                    renderGraph.AddRasterRenderPass<CopyPassData>(
                        $"DLSS-NR Copy Eye {eye}", out CopyPassData passData))
                {
                    passData.Source = source;
                    passData.Material = _feature._copyMaterial;
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

            private static void SetKeyword(Material material, string keyword, bool enabled)
            {
                if (enabled)
                    material.EnableKeyword(keyword);
                else
                    material.DisableKeyword(keyword);
            }

            private static void GetEyePose(UniversalCameraData cameraData, Camera camera,
                int viewIndex, out Vector3 position, out Quaternion rotation,
                out Matrix4x4 projection)
            {
                if (cameraData.xr.enabled)
                {
                    int index = Mathf.Clamp(viewIndex, 0, Mathf.Max(0, cameraData.xr.viewCount - 1));
                    Matrix4x4 invView = cameraData.xr.GetViewMatrix(index).inverse;
                    position = invView.GetColumn(3);
                    rotation = invView.rotation;
                    projection = cameraData.xr.GetProjMatrix(index);
                    return;
                }

                position = camera.transform.position;
                rotation = camera.transform.rotation;
                projection = camera.nonJitteredProjectionMatrix;
            }
        }
    }
}
