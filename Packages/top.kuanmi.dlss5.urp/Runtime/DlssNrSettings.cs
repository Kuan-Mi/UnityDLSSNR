namespace UnityRhi.DlssNr.Urp
{
    /// <summary>A per-camera snapshot of the blended DLSS-NR Volume values.</summary>
    internal readonly struct DlssNrSettings
    {
        internal readonly DlssNrPreset Preset;
        internal readonly DlssNrStyle Style;
        internal readonly float Intensity;
        internal readonly float LocalToneStrength;
        internal readonly float LocalStructureStrength;
        internal readonly float SkinStructureStrength;
        internal readonly bool UseAutoMask;
        internal readonly bool UiCorrection;
        internal readonly UnityEngine.Vector2 MotionVectorScale;
        internal readonly float CameraCutDistance;
        internal readonly float CameraCutAngle;
        internal readonly DlssNrDebugMode DebugMode;
        internal readonly float DebugMotionRange;
        internal readonly float DebugDepthRange;

        internal DlssNrSettings(DlssNrVolume volume)
        {
            Preset = volume.preset.value;
            Style = volume.style.value;
            Intensity = volume.intensity.value;
            LocalToneStrength = volume.localToneStrength.value;
            LocalStructureStrength = volume.localStructureStrength.value;
            SkinStructureStrength = volume.skinStructureStrength.value;
            UseAutoMask = volume.useAutoMask.value;
            UiCorrection = volume.uiCorrection.value;
            MotionVectorScale = volume.motionVectorScale.value;
            CameraCutDistance = volume.cameraCutDistance.value;
            CameraCutAngle = volume.cameraCutAngle.value;
            DebugMode = volume.debugMode.value;
            DebugMotionRange = volume.debugMotionRange.value;
            DebugDepthRange = volume.debugDepthRange.value;
        }
    }
}
