Shader "Hidden/UnityRHI/DLSS-NR/PrepareInputs"
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

            // URP's Core include defines the XR-aware TEXTURE2D_X sampling
            // macros and includes the shared point/linear clamp samplers.
            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

            TEXTURE2D_X(_DlssNrInputColor);
            TEXTURE2D_X(_DlssNrInputDepth);
            TEXTURE2D_X(_DlssNrInputMotion);

            struct Attributes
            {
                uint vertexID : SV_VertexID;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct Varyings
            {
                float4 positionCS : SV_POSITION;
                float2 uv : TEXCOORD0;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            struct Outputs
            {
                float4 color : SV_Target0;
                float2 motion : SV_Target1;
                float depth : SV_Target2;
                float4 fallback : SV_Target3;
            };

            Varyings Vert(Attributes input)
            {
                Varyings output;
                UNITY_SETUP_INSTANCE_ID(input);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(output);
                output.positionCS = GetFullScreenTriangleVertexPosition(input.vertexID);
                output.uv = GetFullScreenTriangleTexCoord(input.vertexID);
                return output;
            }

            Outputs Frag(Varyings input)
            {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(input);
                Outputs output;
                output.color = SAMPLE_TEXTURE2D_X(_DlssNrInputColor,
                    sampler_LinearClamp, input.uv);
                output.fallback = output.color;
                // Preserve URP's raw device depth. Depth inversion is communicated
                // separately to NGX through SystemInfo.usesReversedZBuffer.
                output.depth = SAMPLE_TEXTURE2D_X(_DlssNrInputDepth,
                    sampler_PointClamp, input.uv).r;
                // URP motion is previous-to-current UV/NDC. The C# dispatch scale
                // flips direction and converts it to full-resolution pixels.
                output.motion = SAMPLE_TEXTURE2D_X(_DlssNrInputMotion,
                    sampler_PointClamp, input.uv).xy;
                return output;
            }
            ENDHLSL
        }

        Pass
        {
            Name "DebugInputs"

            HLSLPROGRAM
            #pragma target 4.5
            #pragma vertex VertDebug
            #pragma fragment FragDebug
            #pragma multi_compile _ _USE_DRAW_PROCEDURAL

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

            TEXTURE2D_X(_DlssNrInputDepth);
            TEXTURE2D_X(_DlssNrInputMotion);

            int _DlssNrDebugMode;
            float _DlssNrDebugMotionScaleX;
            float _DlssNrDebugMotionScaleY;
            float _DlssNrDebugMotionRange;
            float _DlssNrDebugDepthRange;

            struct DebugAttributes
            {
                uint vertexID : SV_VertexID;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct DebugVaryings
            {
                float4 positionCS : SV_POSITION;
                float2 uv : TEXCOORD0;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            DebugVaryings VertDebug(DebugAttributes input)
            {
                DebugVaryings output;
                UNITY_SETUP_INSTANCE_ID(input);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(output);
                output.positionCS = GetFullScreenTriangleVertexPosition(input.vertexID);
                output.uv = GetFullScreenTriangleTexCoord(input.vertexID);
                return output;
            }

            float3 MotionHeatmap(float value)
            {
                value = saturate(value);
                return saturate(float3(
                    1.5 - abs(4.0 * value - 3.0),
                    1.5 - abs(4.0 * value - 2.0),
                    1.5 - abs(4.0 * value - 1.0)));
            }

            float4 FragDebug(DebugVaryings input) : SV_Target
            {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(input);

                if (_DlssNrDebugMode == 1 || _DlssNrDebugMode == 2)
                {
                    float2 rawMotion = SAMPLE_TEXTURE2D_X(_DlssNrInputMotion,
                        sampler_PointClamp, input.uv).xy;
                    float2 motionPixels = rawMotion * float2(
                        _DlssNrDebugMotionScaleX, _DlssNrDebugMotionScaleY);
                    float range = max(_DlssNrDebugMotionRange, 1e-4);
                    float magnitude = length(motionPixels) / range;

                    if (_DlssNrDebugMode == 1)
                    {
                        // Zero motion is neutral gray. Red/green encode signed
                        // current-to-previous X/Y motion; blue encodes magnitude.
                        return float4(
                            saturate(0.5 + motionPixels.x / (2.0 * range)),
                            saturate(0.5 + motionPixels.y / (2.0 * range)),
                            saturate(magnitude), 1.0);
                    }

                    return float4(MotionHeatmap(magnitude), 1.0);
                }

                float rawDepth = SAMPLE_TEXTURE2D_X(_DlssNrInputDepth,
                    sampler_PointClamp, input.uv).r;
                if (_DlssNrDebugMode == 3)
                    return float4(rawDepth.xxx, 1.0);

                float eyeDepth = LinearEyeDepth(rawDepth, _ZBufferParams);
                float normalizedDepth = saturate(eyeDepth / max(_DlssNrDebugDepthRange, 1e-4));
                return float4(normalizedDepth.xxx, 1.0);
            }
            ENDHLSL
        }
    }
    Fallback Off
}
