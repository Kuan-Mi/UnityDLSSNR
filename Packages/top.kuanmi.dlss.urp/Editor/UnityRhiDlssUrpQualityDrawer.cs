#if ENABLE_UPSCALER_FRAMEWORK
using System;
using System.Collections.Generic;
using System.Linq.Expressions;
using System.Reflection;
using UnityEditor;
using UnityEditor.Rendering.Universal;
using UnityEngine;
using UnityEngine.Rendering.Universal;

namespace UnityRhi.Dlss.Urp.Editor
{
    /// <summary>
    /// Locks the URP Quality Render Scale slider to the DLSS quality ratio
    /// when UnityRHI DLSS Fixed Resolution Mode is enabled.
    /// </summary>
    [InitializeOnLoad]
    internal static class UnityRhiDlssUrpQualityDrawer
    {
        private static readonly GUIContent RenderScaleContent = EditorGUIUtility.TrTextContent(
            "Render Scale",
            "Scales the camera render target allowing the game to render at a resolution different than native resolution. UI is always rendered at native resolution.");

        private static readonly GUIContent LockedRenderScaleContent = EditorGUIUtility.TrTextContent(
            "Render Scale",
            "Driven by UnityRHI DLSS Quality Mode while Fixed Resolution Mode is enabled.");

        private static readonly MethodInfo OriginalDrawQuality;
        private static readonly MethodInfo DrawHdr;
        private static readonly MethodInfo DrawUpscaling;

        static UnityRhiDlssUrpQualityDrawer()
        {
            try
            {
                Assembly urpEditor = typeof(UniversalRenderPipelineAssetEditor).Assembly;
                Type uiType = urpEditor.GetType(
                    "UnityEditor.Rendering.Universal.UniversalRenderPipelineAssetUI");
                Type serializedType = urpEditor.GetType(
                    "UnityEditor.Rendering.Universal.SerializedUniversalRenderPipelineAsset");
                if (uiType == null || serializedType == null)
                    return;

                OriginalDrawQuality = uiType.GetMethod("DrawQuality",
                    BindingFlags.NonPublic | BindingFlags.Static);
                DrawHdr = uiType.GetMethod("DrawHDR",
                    BindingFlags.NonPublic | BindingFlags.Static);
                DrawUpscaling = uiType.GetMethod("DrawUpscalingFilterDropdownAndOptions",
                    BindingFlags.NonPublic | BindingFlags.Static);
                if (OriginalDrawQuality == null || DrawHdr == null || DrawUpscaling == null)
                    return;

                object inspector = uiType.GetField("Inspector",
                    BindingFlags.Public | BindingFlags.Static)?.GetValue(null);
                if (inspector == null)
                    return;

                if (!TryReplaceDrawQuality(inspector, serializedType, new HashSet<object>()))
                    Debug.LogWarning("[UnityRHI.DLSS] Could not lock the URP Render Scale slider.");
            }
            catch (Exception exception)
            {
                Debug.LogWarning(
                    $"[UnityRHI.DLSS] Could not lock the URP Render Scale slider. {exception.Message}");
            }
        }

        // CED drawers keep ActionDrawer[] on a field or auto-property. Only walk
        // those members — reflecting every property also reaches Type/Assembly
        // graphs and overflows the stack.
        private static bool TryReplaceDrawQuality(object root, Type serializedType, HashSet<object> visited)
        {
            if (root == null || root is Delegate || !visited.Add(root))
                return false;

            const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
            foreach (FieldInfo field in root.GetType().GetFields(flags))
            {
                if (TryReplaceInDrawers(field.GetValue(root), serializedType, visited))
                    return true;
            }

            foreach (PropertyInfo property in root.GetType().GetProperties(flags))
            {
                if (!property.CanRead || property.GetIndexParameters().Length != 0)
                    continue;
                if (property.Name is not ("actionDrawers" or "m_ActionDrawers"))
                    continue;
                if (TryReplaceInDrawers(property.GetValue(root), serializedType, visited))
                    return true;
            }

            return false;
        }

        private static bool TryReplaceInDrawers(object value, Type serializedType, HashSet<object> visited)
        {
            if (value is not Array array)
                return TryReplaceDrawQuality(value, serializedType, visited);

            for (int i = 0; i < array.Length; i++)
            {
                object item = array.GetValue(i);
                if (item is Delegate callback)
                {
                    if (callback.Method == OriginalDrawQuality)
                    {
                        array.SetValue(CreateWrapper(callback.GetType(), serializedType), i);
                        return true;
                    }

                    if (TryReplaceDrawQuality(callback.Target, serializedType, visited))
                        return true;
                    continue;
                }

                if (TryReplaceDrawQuality(item, serializedType, visited))
                    return true;
            }

            return false;
        }

        private static Delegate CreateWrapper(Type actionDrawerType, Type serializedType)
        {
            MethodInfo hook = typeof(UnityRhiDlssUrpQualityDrawer).GetMethod(
                nameof(DrawQuality), BindingFlags.NonPublic | BindingFlags.Static);
            ParameterExpression data = Expression.Parameter(serializedType, "data");
            ParameterExpression owner = Expression.Parameter(typeof(UnityEditor.Editor), "owner");
            MethodCallExpression body = Expression.Call(hook,
                Expression.Convert(data, typeof(object)), owner);
            return Expression.Lambda(actionDrawerType, body, data, owner).Compile();
        }

        private static void DrawQuality(object serialized, UnityEditor.Editor owner)
        {
            if (serialized == null)
            {
                OriginalDrawQuality.Invoke(null, new[] { serialized, owner });
                return;
            }

            Type type = serialized.GetType();
            var renderScale = (SerializedProperty)type.GetProperty("renderScale")?.GetValue(serialized);
            var msaa = (SerializedProperty)type.GetProperty("msaa")?.GetValue(serialized);
            var lod = (SerializedProperty)type.GetProperty("enableLODCrossFadeProp")?.GetValue(serialized);
            var lodType = (SerializedProperty)type.GetProperty("lodCrossFadeDitheringTypeProp")
                ?.GetValue(serialized);
            var asset = type.GetProperty("asset")?.GetValue(serialized) as UniversalRenderPipelineAsset;
            if (renderScale == null || msaa == null || lod == null || lodType == null)
            {
                OriginalDrawQuality.Invoke(null, new[] { serialized, owner });
                return;
            }

            bool lockScale = TryGetLockedScale(asset, out float scale);
            object[] args = { serialized, owner };
            DrawHdr.Invoke(null, args);
            EditorGUILayout.PropertyField(msaa);

            if (lockScale)
                renderScale.floatValue = scale;

            using (new EditorGUI.DisabledScope(lockScale))
            {
                float value = EditorGUILayout.Slider(
                    lockScale ? LockedRenderScaleContent : RenderScaleContent,
                    renderScale.floatValue,
                    UniversalRenderPipeline.minRenderScale,
                    UniversalRenderPipeline.maxRenderScale);
                renderScale.floatValue = lockScale ? scale : value;
            }

            DrawUpscaling.Invoke(null, args);

            if (renderScale.floatValue < 1.0f ||
                asset != null && (asset.upscalingFilter == UpscalingFilterSelection.STP ||
                                  asset.upscalingFilter == UpscalingFilterSelection.FSR))
            {
                EditorGUILayout.HelpBox(
                    "Camera depth isn't supported when Upscaling is turned on in the game view. We will automatically fall back to not doing depth-testing for this pass.",
                    MessageType.Warning, true);
            }

            EditorGUILayout.PropertyField(lod);
            using (new EditorGUI.DisabledScope(!lod.boolValue))
                EditorGUILayout.PropertyField(lodType);
        }

        private static bool TryGetLockedScale(UniversalRenderPipelineAsset asset, out float scale)
        {
            scale = 1.0f;
            if (asset == null || asset.upscalingFilter != UpscalingFilterSelection.IUpscaler)
                return false;
            if (asset.upscalerName != UnityRhiDlssIUpscaler.UpscalerName)
                return false;
            if (asset.GetIUpscalerOptions(UnityRhiDlssIUpscaler.UpscalerName)
                is not UnityRhiDlssOptions options || !options.fixedResolutionMode)
                return false;

            scale = Mathf.Clamp(
                UnityRhiDlssOptions.GetFixedRenderScale(options.qualityMode),
                UniversalRenderPipeline.minRenderScale,
                UniversalRenderPipeline.maxRenderScale);
            return true;
        }
    }
}
#endif
