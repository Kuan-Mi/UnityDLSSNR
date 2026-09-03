#if ENABLE_UPSCALER_FRAMEWORK
using System;
using UnityEngine;
using UnityEngine.Rendering;

namespace UnityRhi.Dlss.Urp
{
    /// <summary>Serialized options for the UnityRHI DLSS IUpscaler (URP Asset).</summary>
    [Serializable]
    public sealed class UnityRhiDlssOptions : UpscalerOptions
    {
        [Tooltip("DLSS performance / quality mode. NATIVE is DLAA (full internal resolution).")]
        public UpscalerMode qualityMode = UpscalerMode.QUALITY;

        [Tooltip("On: internal resolution comes from Quality Mode and URP Render Scale is locked. Off: Quality Mode is hidden and Render Scale drives the DLSS input size.")]
        public bool fixedResolutionMode = true;

        [Tooltip("NGX network preset hint for the active quality mode.")]
        public DlssPreset preset = DlssPreset.Default;

        [HideInInspector]
        public Vector2 motionVectorScale = Vector2.one;

        [SerializeField, HideInInspector, Tooltip(
            "Input preparation shader. Automatically resolved from the package so Player builds keep it.")]
        public UnityEngine.Shader prepareInputsShader;

        /// <summary>
        /// URP Render Scale implied by a DLSS quality mode. Prefers NGX optimal
        /// settings when the plugin is available, otherwise NVIDIA's nominal ratios.
        /// </summary>
        public static float GetFixedRenderScale(UpscalerMode mode)
        {
            if (mode == UpscalerMode.NATIVE)
                return 1.0f;

            mode = SanitizeQualityMode(mode);

            const int referenceWidth = 1920;
            const int referenceHeight = 1080;
            if (RhiCore.QueryDlssOptimalSettings(referenceWidth, referenceHeight, mode,
                    out int renderWidth, out _))
            {
                return Mathf.Clamp((float)renderWidth / referenceWidth, 0.1f, 1.0f);
            }

            return mode switch
            {
                UpscalerMode.QUALITY => 0.67f,
                UpscalerMode.BALANCED => 0.58f,
                UpscalerMode.PERFORMANCE => 0.50f,
                UpscalerMode.ULTRA_PERFORMANCE => 0.33f,
                _ => 1.0f,
            };
        }

        /// <summary>
        /// NGX SuperSampling does not implement Ultra Quality on current
        /// nvngx_dlss snippets. Map it to Quality so old assets keep working.
        /// </summary>
        public static UpscalerMode SanitizeQualityMode(UpscalerMode mode) =>
            mode == UpscalerMode.ULTRA_QUALITY ? UpscalerMode.QUALITY : mode;
    }
}
#endif
