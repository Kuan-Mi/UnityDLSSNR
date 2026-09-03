Shader "Hidden/UnityRHI/DLSS-G/PrepareInputs"
{
    SubShader
    {
        Tags { "RenderPipeline" = "UniversalPipeline" }
        ZTest Always
        ZWrite Off
        Cull Off

        Pass
        {
            Name "PrepareInputs"

            HLSLPROGRAM
            #pragma target 4.5
            #pragma vertex Vert
            #pragma fragment Frag
            #pragma multi_compile _ _USE_DRAW_PROCEDURAL

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

            TEXTURE2D(_DlssFgInputDepth);
            TEXTURE2D(_DlssFgInputMotion);

            struct Attributes
            {
                uint vertexID : SV_VertexID;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct Varyings
            {
                float4 positionCS : SV_POSITION;
                float2 uv : TEXCOORD0;
            };

            struct Outputs
            {
                float2 motion : SV_Target0;
                float depth : SV_Target1;
            };

            Varyings Vert(Attributes input)
            {
                Varyings output;
                UNITY_SETUP_INSTANCE_ID(input);
                output.positionCS = GetFullScreenTriangleVertexPosition(input.vertexID);
                output.uv = GetFullScreenTriangleTexCoord(input.vertexID);
                return output;
            }

            Outputs Frag(Varyings input)
            {
                Outputs output;
                output.motion = SAMPLE_TEXTURE2D(_DlssFgInputMotion,
                    sampler_PointClamp, input.uv).xy;
                output.depth = SAMPLE_TEXTURE2D(_DlssFgInputDepth,
                    sampler_PointClamp, input.uv).r;
                return output;
            }
            ENDHLSL
        }
    }
    Fallback Off
}
