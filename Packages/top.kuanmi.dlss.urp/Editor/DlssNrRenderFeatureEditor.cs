using UnityEditor;
using UnityEngine;

namespace UnityRhi.Dlss.Urp.Editor
{
    [CustomEditor(typeof(DlssNrRenderFeature))]
    internal sealed class DlssNrRenderFeatureEditor : UnityEditor.Editor
    {
        private SerializedProperty _shader;

        private void OnEnable()
        {
            _shader = serializedObject.FindProperty("prepareInputsShader");
            AssignDefaultShader();
        }

        public override void OnInspectorGUI()
        {
            AssignDefaultShader();
            DrawDefaultInspector();
            if (GUILayout.Button("Reset DLSS-NR History"))
            {
                var feature = (DlssNrRenderFeature)target;
                feature.ResetHistory();
            }
            EditorGUILayout.Space();
            EditorGUILayout.HelpBox(
                "SDR Game-camera order: raster → NR → remaining post. " +
                "XR is supported at native resolution with per-eye history " +
                "(multipass or single-pass instanced). Keep Render Pass Event at " +
                "Before Rendering Post Processing unless you want post → NR.",
                MessageType.Info);
            using (new EditorGUI.DisabledScope(!RhiCore.IsD3D12Active))
            {
                EditorGUILayout.LabelField("Runtime Status", EditorStyles.boldLabel);
                EditorGUILayout.LabelField("D3D12", RhiCore.IsD3D12Active ? "Active" : "Unavailable");
                EditorGUILayout.LabelField("DLSS-NR Runtime", RhiCore.IsDlssNrAvailable
                    ? "Available" : $"Unavailable (0x{unchecked((uint)RhiCore.DlssNrInitResult):X8})");
                EditorGUILayout.LabelField("Last Create", FormatResult(RhiCore.DlssNrLastCreateResult));
                EditorGUILayout.LabelField("Last Evaluate", FormatResult(RhiCore.DlssNrLastEvaluateResult));
            }
            EditorGUILayout.HelpBox(
                "Requires Unity 6.3 URP, Windows x64, Direct3D 12 and supported NVIDIA hardware/driver. " +
                "This path is SDR (no HDR Output). XR uses native-resolution per-eye evaluation.",
                MessageType.Info);
        }

        private void AssignDefaultShader()
        {
            if (_shader == null || _shader.objectReferenceValue != null)
                return;
            UnityEngine.Shader shader = AssetDatabase.LoadAssetAtPath<UnityEngine.Shader>(
                "Packages/top.kuanmi.dlss.urp/Shaders/DlssNrPrepareInputs.shader");
            if (shader == null)
                return;
            serializedObject.Update();
            _shader.objectReferenceValue = shader;
            serializedObject.ApplyModifiedPropertiesWithoutUndo();
            EditorUtility.SetDirty(target);
        }

        private static string FormatResult(int result) =>
            $"0x{unchecked((uint)result):X8}";
    }
}
