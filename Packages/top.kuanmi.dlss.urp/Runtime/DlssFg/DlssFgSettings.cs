namespace UnityRhi.Dlss.Urp
{
    /// <summary>A per-camera snapshot of the renderer-feature DLSS-G settings.</summary>
    internal readonly struct DlssFgSettings
    {
        internal readonly UnityEngine.Vector2 MotionVectorScale;
        internal readonly float CameraCutDistance;
        internal readonly float CameraCutAngle;

        internal DlssFgSettings(DlssFgRenderFeature feature)
        {
            MotionVectorScale = feature.motionVectorScale;
            CameraCutDistance = feature.cameraCutDistance;
            CameraCutAngle = feature.cameraCutAngle;
        }
    }
}
