#if ENABLE_UPSCALER_FRAMEWORK
using UnityEngine;
using UnityEngine.Rendering;

#if UNITY_EDITOR
using UnityEditor;
#endif

namespace UnityRhi.Dlss.Urp
{
#if UNITY_EDITOR
    [InitializeOnLoad]
#endif
    static class RegisterUnityRhiDlss
    {
        static RegisterUnityRhiDlss() =>
            UpscalerRegistry.Register<UnityRhiDlssIUpscaler, UnityRhiDlssOptions>(
                UnityRhiDlssIUpscaler.UpscalerName);

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        static void InitRuntime() =>
            UpscalerRegistry.Register<UnityRhiDlssIUpscaler, UnityRhiDlssOptions>(
                UnityRhiDlssIUpscaler.UpscalerName);
    }
}
#endif
