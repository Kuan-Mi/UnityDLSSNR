#if ENABLE_UPSCALER_FRAMEWORK
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering.Universal;
using UnityRhi;

namespace UnityRhi.Dlss.Urp.Editor
{
    [CustomEditor(typeof(UnityRhiDlssOptions))]
    internal sealed class UnityRhiDlssOptionsEditor : UnityEditor.Editor
    {
        private SerializedProperty _qualityMode;
        private SerializedProperty _fixedResolutionMode;
        private SerializedProperty _preset;

        private void OnEnable()
        {
            _qualityMode = serializedObject.FindProperty("qualityMode");
            _fixedResolutionMode = serializedObject.FindProperty("fixedResolutionMode");
            _preset = serializedObject.FindProperty("preset");
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();
            EditorGUILayout.PropertyField(_fixedResolutionMode);
            if (_fixedResolutionMode.boolValue)
                DrawQualityMode();
            EditorGUILayout.PropertyField(_preset);
            serializedObject.ApplyModifiedProperties();

            if (target is UnityRhiDlssOptions options && options.fixedResolutionMode)
                ApplyFixedRenderScale(options);
        }

        private static readonly UpscalerMode[] QualityModes =
        {
            UpscalerMode.NATIVE,
            UpscalerMode.QUALITY,
            UpscalerMode.BALANCED,
            UpscalerMode.PERFORMANCE,
            UpscalerMode.ULTRA_PERFORMANCE,
        };

        private static readonly GUIContent[] QualityModeLabels =
        {
            new("Native (DLAA)"),
            new("Quality"),
            new("Balanced"),
            new("Performance"),
            new("Ultra Performance"),
        };

        private void DrawQualityMode()
        {
            UpscalerMode current = UnityRhiDlssOptions.SanitizeQualityMode(
                (UpscalerMode)_qualityMode.enumValueIndex);
            int selected = System.Array.IndexOf(QualityModes, current);
            if (selected < 0)
                selected = System.Array.IndexOf(QualityModes, UpscalerMode.QUALITY);

            int next = EditorGUILayout.Popup(
                new GUIContent(_qualityMode.displayName), selected, QualityModeLabels);
            _qualityMode.enumValueIndex = (int)QualityModes[next];
        }

        private static void ApplyFixedRenderScale(UnityRhiDlssOptions options)
        {
            float scale = UnityRhiDlssOptions.GetFixedRenderScale(options.qualityMode);
            scale = Mathf.Clamp(scale,
                UniversalRenderPipeline.minRenderScale,
                UniversalRenderPipeline.maxRenderScale);

            string path = AssetDatabase.GetAssetPath(options);
            if (string.IsNullOrEmpty(path))
                return;

            UniversalRenderPipelineAsset asset =
                AssetDatabase.LoadAssetAtPath<UniversalRenderPipelineAsset>(path);
            if (asset == null)
                return;

            foreach (UnityEditor.Editor editor in ActiveEditorTracker.sharedTracker.activeEditors)
            {
                if (editor == null || editor.target != asset)
                    continue;

                SerializedProperty renderScale = editor.serializedObject.FindProperty("m_RenderScale");
                if (renderScale == null || Mathf.Approximately(renderScale.floatValue, scale))
                    return;

                renderScale.floatValue = scale;
                return;
            }

            if (Mathf.Approximately(asset.renderScale, scale))
                return;

            Undo.RecordObject(asset, "DLSS Render Scale");
            asset.renderScale = scale;
            EditorUtility.SetDirty(asset);
        }
    }
}
#endif
