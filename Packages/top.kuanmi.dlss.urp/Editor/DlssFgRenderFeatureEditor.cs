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
            EditorGUILayout.Space();
            EditorGUILayout.HelpBox(
                "DLSS Frame Generation inserts interpolated frames at Present. " +
                "Enable it on this renderer feature (or F8 in the Player HUD). " +
                "Frame insertion is Player-only; Debug View works in the Editor Game view. No Volume is used. " +
                "The feature clears VSync/target frame rate while active. Keep Render Pass " +
                "Event at After Rendering Post Processing. XR is not supported.",
                MessageType.Info);
            EditorGUILayout.HelpBox(
                "DLSS-G input contract: depth = hardware/device depth (R32 float here); " +
                "motion = current-to-previous, top-left screen coordinates (RG16 float here). " +
                "URP normalized UV motion is converted to pixels through MvecScale. " +
                "Motion Error should be green for a static scene while the camera moves; " +
                "red means the submitted motion and ClipToPrevClip prediction disagree. " +
                "Blue marks invalid/background depth.",
                MessageType.Info);
            using (new EditorGUI.DisabledScope(!RhiCore.IsD3D12Active))
            {
                EditorGUILayout.LabelField("Runtime Status", EditorStyles.boldLabel);
                EditorGUILayout.LabelField("D3D12", RhiCore.IsD3D12Active ? "Active" : "Unavailable");
                EditorGUILayout.LabelField("DLSS-G Runtime", RhiCore.IsNgxFrameGenerationAvailable
                    ? "Available" : $"Unavailable (0x{unchecked((uint)RhiCore.NgxFrameGenerationInitResult):X8})");
                EditorGUILayout.LabelField("Max generated frames",
                    RhiCore.NgxFrameGenerationMultiFrameCountMax.ToString());
            }
            EditorGUILayout.HelpBox(
                "Requires Unity 6.3 URP, Windows x64, Direct3D 12, a standalone Player, " +
                "and an RTX 40-series or newer GPU/driver. Works with native resolution " +
                "or UnityRHI DLSS Super Resolution (depth/MV stay at render resolution).",
                MessageType.Info);
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
