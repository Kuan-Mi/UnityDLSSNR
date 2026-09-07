using UnityEditor;
using UnityEngine;

namespace UnityRhi.Dlss.Urp.Editor
{
    [CustomEditor(typeof(DlssFgRenderFeature))]
    internal sealed class DlssFgRenderFeatureEditor : UnityEditor.Editor
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
            if (GUILayout.Button("Reset DLSS-G History"))
            {
                var feature = (DlssFgRenderFeature)target;
                feature.ResetHistory();
            }


            using (new EditorGUI.DisabledScope(!RhiCore.IsD3D12Active))
            {
                EditorGUILayout.LabelField("Runtime Status", EditorStyles.boldLabel);
                EditorGUILayout.LabelField("D3D12", RhiCore.IsD3D12Active ? "Active" : "Unavailable");
                EditorGUILayout.LabelField("DLSS-G Runtime", RhiCore.IsNgxFrameGenerationAvailable
                    ? "Available" : $"Unavailable (0x{unchecked((uint)RhiCore.NgxFrameGenerationInitResult):X8})");
                EditorGUILayout.LabelField("Max generated frames",
                    RhiCore.NgxFrameGenerationMultiFrameCountMax.ToString());
            }
        }

        private void AssignDefaultShader()
        {
            if (_shader == null || _shader.objectReferenceValue != null)
                return;
            UnityEngine.Shader shader = AssetDatabase.LoadAssetAtPath<UnityEngine.Shader>(
                "Packages/top.kuanmi.dlss.urp/Shaders/DlssFgPrepareInputs.shader");
            if (shader == null)
                return;
            serializedObject.Update();
            _shader.objectReferenceValue = shader;
            serializedObject.ApplyModifiedPropertiesWithoutUndo();
            EditorUtility.SetDirty(target);
        }
    }
}
