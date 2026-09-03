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
            #pragma multi_compile _ _DLSSNR_COLOR_ARRAY
            #pragma multi_compile _ _DLSSNR_DEPTH_ARRAY
            #pragma multi_compile _ _DLSSNR_MOTION_ARRAY

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

            #if defined(_DLSSNR_COLOR_ARRAY)
            TEXTURE2D_ARRAY(_DlssNrInputColor);
            #else
            TEXTURE2D(_DlssNrInputColor);
            #endif
            #if defined(_DLSSNR_DEPTH_ARRAY)
            TEXTURE2D_ARRAY(_DlssNrInputDepth);
            #else
            TEXTURE2D(_DlssNrInputDepth);
            #endif
            #if defined(_DLSSNR_MOTION_ARRAY)
            TEXTURE2D_ARRAY(_DlssNrInputMotion);
            #else
            TEXTURE2D(_DlssNrInputMotion);
            #endif
            float _DlssNrEyeSlice;

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
                float4 color : SV_Target0;
                float2 motion : SV_Target1;
                float depth : SV_Target2;
                float4 fallback : SV_Target3;
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
                float slice = _DlssNrEyeSlice;
                #if defined(_DLSSNR_COLOR_ARRAY)
                output.color = SAMPLE_TEXTURE2D_ARRAY(_DlssNrInputColor,
                    sampler_LinearClamp, input.uv, slice);
                #else
                output.color = SAMPLE_TEXTURE2D(_DlssNrInputColor,
                    sampler_LinearClamp, input.uv);
                #endif
                #if defined(_DLSSNR_DEPTH_ARRAY)
                output.depth = SAMPLE_TEXTURE2D_ARRAY(_DlssNrInputDepth,
                    sampler_PointClamp, input.uv, slice).r;
                #else
                output.depth = SAMPLE_TEXTURE2D(_DlssNrInputDepth,
                    sampler_PointClamp, input.uv).r;
                #endif
                #if defined(_DLSSNR_MOTION_ARRAY)
                output.motion = SAMPLE_TEXTURE2D_ARRAY(_DlssNrInputMotion,
                    sampler_LinearClamp, input.uv, slice).xy;
                #else
                output.motion = SAMPLE_TEXTURE2D(_DlssNrInputMotion,
                    sampler_LinearClamp, input.uv).xy;
                #endif
                output.fallback = output.color;
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

            TEXTURE2D(_DlssNrInputDepth);
            TEXTURE2D(_DlssNrInputMotion);

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
            };

            DebugVaryings VertDebug(DebugAttributes input)
            {
                DebugVaryings output;
                UNITY_SETUP_INSTANCE_ID(input);
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
                if (_DlssNrDebugMode == 1 || _DlssNrDebugMode == 2)
                {
                    float2 rawMotion = SAMPLE_TEXTURE2D(_DlssNrInputMotion,
                        sampler_PointClamp, input.uv).xy;
                    float2 motionPixels = rawMotion * float2(
                        _DlssNrDebugMotionScaleX, _DlssNrDebugMotionScaleY);
                    float range = max(_DlssNrDebugMotionRange, 1e-4);
                    float magnitude = length(motionPixels) / range;

                    if (_DlssNrDebugMode == 1)
                    {
                        return float4(
                            saturate(0.5 + motionPixels.x / (2.0 * range)),
                            saturate(0.5 + motionPixels.y / (2.0 * range)),
                            saturate(magnitude), 1.0);
                    }

                    return float4(MotionHeatmap(magnitude), 1.0);
                }

                float rawDepth = SAMPLE_TEXTURE2D(_DlssNrInputDepth,
                    sampler_PointClamp, input.uv).r;
                if (_DlssNrDebugMode == 3)
                    return float4(rawDepth.xxx, 1.0);

                float eyeDepth = LinearEyeDepth(rawDepth, _ZBufferParams);
                float normalizedDepth = saturate(eyeDepth / max(_DlssNrDebugDepthRange, 1e-4));
                return float4(normalizedDepth.xxx, 1.0);
            }
            ENDHLSL
        }

        Pass
        {
            Name "CopyToSlice"

            HLSLPROGRAM
            #pragma target 4.5
            #pragma vertex VertCopy
            #pragma fragment FragCopy
            #pragma multi_compile _ _USE_DRAW_PROCEDURAL

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

            TEXTURE2D(_DlssNrCopySource);

            struct CopyAttributes
            {
                uint vertexID : SV_VertexID;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct CopyVaryings
            {
                float4 positionCS : SV_POSITION;
                float2 uv : TEXCOORD0;
            };

            CopyVaryings VertCopy(CopyAttributes input)
            {
                CopyVaryings output;
                UNITY_SETUP_INSTANCE_ID(input);
                output.positionCS = GetFullScreenTriangleVertexPosition(input.vertexID);
                output.uv = GetFullScreenTriangleTexCoord(input.vertexID);
                return output;
            }

            float4 FragCopy(CopyVaryings input) : SV_Target
            {
                return SAMPLE_TEXTURE2D(_DlssNrCopySource, sampler_LinearClamp, input.uv);
            }
            ENDHLSL
        }
    }
    Fallback Off
}
