using System;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

namespace UnityRhi.Dlss.Urp
{
    public enum DlssNrDebugMode
    {
        Off,
        MotionVectors,
        MotionMagnitude,
        DeviceDepth,
        LinearEyeDepth,
    }

    [Serializable]
    public sealed class DlssNrPresetParameter : VolumeParameter<DlssNrPreset>
    {
        public DlssNrPresetParameter(DlssNrPreset value, bool overrideState = false)
            : base(value, overrideState) { }
    }

    [Serializable]
    public sealed class DlssNrStyleParameter : VolumeParameter<DlssNrStyle>
    {
        public DlssNrStyleParameter(DlssNrStyle value, bool overrideState = false)
            : base(value, overrideState) { }
    }

    [Serializable]
    public sealed class DlssNrDebugModeParameter : VolumeParameter<DlssNrDebugMode>
    {
        public DlssNrDebugModeParameter(DlssNrDebugMode value, bool overrideState = false)
            : base(value, overrideState) { }
    }

    /// <summary>Camera-blended runtime controls for the URP DLSS-NR post-process.</summary>
    [Serializable, VolumeComponentMenu("Post-processing/DLSS Neural Rendering")]
    public sealed class DlssNrVolume : VolumeComponent, IPostProcessComponent
    {
        [Tooltip("Enable the DLSS Neural Rendering post-process.")]
        public BoolParameter enabled = new BoolParameter(false);

        public DlssNrPresetParameter preset = new DlssNrPresetParameter(DlssNrPreset.Default);
        public DlssNrStyleParameter style = new DlssNrStyleParameter(DlssNrStyle.Default);

        [Tooltip("Overall Neural Rendering intensity.")]
        public ClampedFloatParameter intensity = new ClampedFloatParameter(1f, 0f, 2f);

        public ClampedFloatParameter localToneStrength = new ClampedFloatParameter(1f, 0f, 2f);
        public ClampedFloatParameter localStructureStrength = new ClampedFloatParameter(1f, 0f, 2f);
        public ClampedFloatParameter skinStructureStrength = new ClampedFloatParameter(-1f, -1f, 2f);

        public BoolParameter useAutoMask = new BoolParameter(false);
        public BoolParameter uiCorrection = new BoolParameter(false);

        [Tooltip("Additional multiplier after URP motion is converted to current-to-previous pixel motion.")]
        public Vector2Parameter motionVectorScale = new Vector2Parameter(Vector2.one);

        [Tooltip("Camera movement larger than this in one rendered frame resets temporal history.")]
        public MinFloatParameter cameraCutDistance = new MinFloatParameter(5f, 0f);

        [Tooltip("Camera rotation larger than this in one rendered frame resets temporal history.")]
        public ClampedFloatParameter cameraCutAngle = new ClampedFloatParameter(45f, 0f, 180f);

        [Header("Input Debug")]
        [Tooltip("Visualize the prepared input that would be submitted to NGX. Debug modes bypass DLSS-NR evaluation.")]
        public DlssNrDebugModeParameter debugMode =
            new DlssNrDebugModeParameter(DlssNrDebugMode.Off);

        [Tooltip("Motion in pixels that reaches the edge of the direction view or maximum of the magnitude view.")]
        public ClampedFloatParameter debugMotionRange = new ClampedFloatParameter(32f, 1f, 256f);

        [Tooltip("Eye-space distance represented as white in the Linear Eye Depth view.")]
        public MinFloatParameter debugDepthRange = new MinFloatParameter(100f, 0.01f);

        public bool IsActive() => active && enabled.value;

        public bool IsTileCompatible() => false;
    }
}
