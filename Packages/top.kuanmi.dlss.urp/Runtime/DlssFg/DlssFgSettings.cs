namespace UnityRhi.Dlss.Urp
{
    /// <summary>A per-camera snapshot of the renderer-feature DLSS-G settings.</summary>
    internal readonly struct DlssFgSettings
    {
        internal readonly float CameraCutDistance;
        internal readonly float CameraCutAngle;

        internal DlssFgSettings(DlssFgRenderFeature feature)
        {
            CameraCutDistance = feature.cameraCutDistance;
            CameraCutAngle = feature.cameraCutAngle;
        }
    }
}
