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
        private SerializedProperty _prepareInputsShader;

        private void OnEnable()
        {
            _qualityMode = serializedObject.FindProperty("qualityMode");
            _fixedResolutionMode = serializedObject.FindProperty("fixedResolutionMode");
            _preset = serializedObject.FindProperty("preset");
            _prepareInputsShader = serializedObject.FindProperty("prepareInputsShader");
            AssignDefaultShader();
        }

        public override void OnInspectorGUI()
        {
            AssignDefaultShader();
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

        private void AssignDefaultShader()
        {
            if (_prepareInputsShader == null || _prepareInputsShader.objectReferenceValue != null)
                return;
            UnityEngine.Shader shader = AssetDatabase.LoadAssetAtPath<UnityEngine.Shader>(
                "Packages/top.kuanmi.dlss.urp/Shaders/DlssPrepareInputs.shader");
            if (shader == null)
                return;
            serializedObject.Update();
            _prepareInputsShader.objectReferenceValue = shader;
            serializedObject.ApplyModifiedPropertiesWithoutUndo();
            EditorUtility.SetDirty(target);
        }
    }
}
#endif
