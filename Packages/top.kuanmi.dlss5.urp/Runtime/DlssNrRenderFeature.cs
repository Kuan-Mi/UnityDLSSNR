using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.Universal;

namespace UnityRhi.DlssNr.Urp
{
    /// <summary>
    /// Full-resolution DLSS Neural Rendering post-process for Unity 6 URP.
    /// It consumes raster color, depth and motion vectors and has no RTXPT dependency.
    /// </summary>
    [DisallowMultipleRendererFeature("DLSS Neural Rendering")]
    public sealed class DlssNrRenderFeature : ScriptableRendererFeature
    {
        [Tooltip("The pass should remain after URP post-processing so DLSS-NR receives display-referred color.")]
        public RenderPassEvent renderPassEvent = RenderPassEvent.AfterRenderingPostProcessing;

        [Tooltip("Allow the effect in the Scene view when its Volume is active.")]
        public bool runInSceneView;

        [Tooltip("Only process the camera that resolves the final camera-stack target.")]
        public bool finalCameraInStackOnly = true;

        [SerializeField, Tooltip("Input preparation shader. Automatically resolved from the package when possible.")]
        private UnityEngine.Shader prepareInputsShader;

        private Material _prepareMaterial;
        private Material _debugMaterial;
        private DlssNrPass _pass;
        private readonly Dictionary<Camera, DlssNrCameraContext> _contexts =
            new Dictionary<Camera, DlssNrCameraContext>();
        private readonly List<Camera> _deadCameras = new List<Camera>();
        private bool _warnedUnavailable;
        private bool _warnedHdr;
        private bool _warnedXr;
        private bool _warnedFailure;

        public override void Create()
        {
            if (prepareInputsShader == null)
                prepareInputsShader = UnityEngine.Shader.Find("Hidden/UnityRHI/DLSS-NR/PrepareInputs");
            if (_prepareMaterial != null)
                CoreUtils.Destroy(_prepareMaterial);
            if (_debugMaterial != null)
                CoreUtils.Destroy(_debugMaterial);
            _prepareMaterial = prepareInputsShader != null
                ? CoreUtils.CreateEngineMaterial(prepareInputsShader) : null;
            // RenderGraph records the prepare and debug draws separately. Keeping
            // independent materials prevents the debug pass texture bindings from
            // mutating the material referenced by the earlier prepare draw.
            _debugMaterial = prepareInputsShader != null
                ? CoreUtils.CreateEngineMaterial(prepareInputsShader) : null;
            _pass = new DlssNrPass(this) { renderPassEvent = renderPassEvent };
#if UNITY_EDITOR
            RhiDomainReload.RegisterOwner(this);
#endif
        }

        public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData renderingData)
        {
            if (_prepareMaterial == null || _debugMaterial == null)
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
            _deadCameras.Clear();
            if (_prepareMaterial != null)
                CoreUtils.Destroy(_prepareMaterial);
            if (_debugMaterial != null)
                CoreUtils.Destroy(_debugMaterial);
            _prepareMaterial = null;
            _debugMaterial = null;
            _pass = null;
            base.Dispose(disposing);
        }

        private DlssNrCameraContext GetContext(Camera camera, int width, int height)
        {
            PruneDeadCameras();
            if (_contexts.TryGetValue(camera, out DlssNrCameraContext context))
            {
                if (context.Width == width && context.Height == height)
                    return context;
                context.Dispose();
                _contexts.Remove(camera);
            }

            context = new DlssNrCameraContext(width, height, camera.name);
            _contexts.Add(camera, context);
            return context;
        }

        private void PruneDeadCameras()
        {
            _deadCameras.Clear();
            foreach (KeyValuePair<Camera, DlssNrCameraContext> pair in _contexts)
                if (pair.Key == null)
                    _deadCameras.Add(pair.Key);
            foreach (Camera camera in _deadCameras)
            {
                _contexts[camera].Dispose();
                _contexts.Remove(camera);
            }
            _deadCameras.Clear();
        }

        private sealed class DlssNrPass : ScriptableRenderPass
        {
            private static readonly int InputColorId = UnityEngine.Shader.PropertyToID("_DlssNrInputColor");
            private static readonly int InputDepthId = UnityEngine.Shader.PropertyToID("_DlssNrInputDepth");
            private static readonly int InputMotionId = UnityEngine.Shader.PropertyToID("_DlssNrInputMotion");
            private static readonly int DebugModeId = UnityEngine.Shader.PropertyToID("_DlssNrDebugMode");
            private static readonly int DebugMotionScaleXId = UnityEngine.Shader.PropertyToID("_DlssNrDebugMotionScaleX");
            private static readonly int DebugMotionScaleYId = UnityEngine.Shader.PropertyToID("_DlssNrDebugMotionScaleY");
            private static readonly int DebugMotionRangeId = UnityEngine.Shader.PropertyToID("_DlssNrDebugMotionRange");
            private static readonly int DebugDepthRangeId = UnityEngine.Shader.PropertyToID("_DlssNrDebugDepthRange");
            private readonly DlssNrRenderFeature _feature;

            private sealed class PreparePassData
            {
                public TextureHandle Color;
                public TextureHandle Depth;
                public TextureHandle Motion;
                public Material Material;
            }

            private sealed class DispatchPassData
            {
                public DlssNrRenderFeature Feature;
                public Camera Camera;
                public DlssNrCameraContext Context;
                public DlssNrCameraContext.DispatchParameters Parameters;
            }

            private sealed class DebugPassData
            {
                public TextureHandle Depth;
                public TextureHandle Motion;
                public Material Material;
                public int Mode;
                public float MotionScaleX;
                public float MotionScaleY;
                public float MotionRange;
                public float DepthRange;
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
                        Debug.LogWarning("[UnityRHI.DLSS-NR] HDR Output is not supported by the full-resolution SDR post-process; bypassing.");
                    }
                    return;
                }
                if (cameraData.xr.enabled)
                {
                    if (!_feature._warnedXr)
                    {
                        _feature._warnedXr = true;
                        Debug.LogWarning("[UnityRHI.DLSS-NR] XR texture arrays are not supported yet; bypassing.");
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

                DlssNrCameraContext context;
                try
                {
                    context = _feature.GetContext(camera, width, height);
                }
                catch (Exception exception)
                {
                    if (!_feature._warnedFailure)
                    {
                        _feature._warnedFailure = true;
                        Debug.LogError($"[UnityRHI.DLSS-NR] Resource initialization failed; bypassing. {exception}");
                    }
                    return;
                }

                TextureHandle color = renderGraph.ImportTexture(context.ColorHandle);
                TextureHandle depth = renderGraph.ImportTexture(context.DepthHandle);
                TextureHandle motion = renderGraph.ImportTexture(context.MotionHandle);
                TextureHandle output = renderGraph.ImportTexture(context.OutputHandle);

                using (IRasterRenderGraphBuilder builder =
                    renderGraph.AddRasterRenderPass<PreparePassData>("DLSS-NR Prepare Inputs",
                        out PreparePassData passData))
                {
                    passData.Color = sourceColor;
                    passData.Depth = sourceDepth;
                    passData.Motion = sourceMotion;
                    passData.Material = _feature._prepareMaterial;
                    builder.UseTexture(sourceColor, AccessFlags.Read);
                    builder.UseTexture(sourceDepth, AccessFlags.Read);
                    builder.UseTexture(sourceMotion, AccessFlags.Read);
                    builder.SetRenderAttachment(color, 0, AccessFlags.WriteAll);
                    builder.SetRenderAttachment(motion, 1, AccessFlags.WriteAll);
                    builder.SetRenderAttachment(depth, 2, AccessFlags.WriteAll);
                    // Seed the output with an exact color fallback. The following
                    // unsafe pass transitions it from RT to UAV before NGX writes it.
                    builder.SetRenderAttachment(output, 3, AccessFlags.WriteAll);
                    builder.AllowPassCulling(false);
                    builder.SetRenderFunc(static (PreparePassData data, RasterGraphContext rgContext) =>
                    {
                        data.Material.SetTexture(InputColorId, data.Color);
                        data.Material.SetTexture(InputDepthId, data.Depth);
                        data.Material.SetTexture(InputMotionId, data.Motion);
                        rgContext.cmd.DrawProcedural(Matrix4x4.identity, data.Material, 0,
                            MeshTopology.Triangles, 3, 1);
                    });
                }

                if (settings.DebugMode != DlssNrDebugMode.Off)
                {
                    using (IRasterRenderGraphBuilder builder =
                        renderGraph.AddRasterRenderPass<DebugPassData>("DLSS-NR Debug Inputs",
                            out DebugPassData passData, profilingSampler))
                    {
                        passData.Depth = depth;
                        passData.Motion = motion;
                        passData.Material = _feature._debugMaterial;
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
                            data.Material.SetTexture(InputDepthId, data.Depth);
                            data.Material.SetTexture(InputMotionId, data.Motion);
                            data.Material.SetInt(DebugModeId, data.Mode);
                            data.Material.SetFloat(DebugMotionScaleXId, data.MotionScaleX);
                            data.Material.SetFloat(DebugMotionScaleYId, data.MotionScaleY);
                            data.Material.SetFloat(DebugMotionRangeId, data.MotionRange);
                            data.Material.SetFloat(DebugDepthRangeId, data.DepthRange);
                            rgContext.cmd.DrawProcedural(Matrix4x4.identity, data.Material, 1,
                                MeshTopology.Triangles, 3, 1);
                        });
                    }

                    // Debug shows the exact prepared inputs and deliberately skips
                    // NGX so its output cannot obscure an input-contract problem.
                    resources.cameraColor = output;
                    return;
                }

                using (IUnsafeRenderGraphBuilder builder =
                    renderGraph.AddUnsafePass<DispatchPassData>("DLSS Neural Rendering",
                        out DispatchPassData passData, profilingSampler))
                {
                    passData.Feature = _feature;
                    passData.Camera = camera;
                    passData.Context = context;
                    passData.Parameters = context.BeginFrame(camera, Time.frameCount, settings);
                    // Inputs are intentionally hidden from RenderGraph here. The MRT
                    // leaves them in RenderTarget state, which is the initial state of
                    // their UnityRHI wrappers; the native dispatch performs and restores
                    // its own RT -> SRV transitions. Output is a declared UAV write.
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

                resources.cameraColor = output;
            }
        }
    }
}
