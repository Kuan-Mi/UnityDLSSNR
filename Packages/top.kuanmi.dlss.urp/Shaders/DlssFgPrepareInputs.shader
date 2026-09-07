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
            float4 _DlssFgDepthScaleBias;
            float4 _DlssFgMotionScaleBias;

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
                float2 depthUv = input.uv * _DlssFgDepthScaleBias.xy +
                    _DlssFgDepthScaleBias.zw;
                float2 motionUv = input.uv * _DlssFgMotionScaleBias.xy +
                    _DlssFgMotionScaleBias.zw;
                output.motion = SAMPLE_TEXTURE2D(_DlssFgInputMotion,
                    sampler_PointClamp, motionUv).xy;
                // Converting a vector field from bottom-left to top-left
                // coordinates requires both reversing its rows and negating
                // the vector's Y component. Depth is a scalar and only needs
                // the sampling-coordinate flip.
                if (_DlssFgMotionScaleBias.y < 0.0)
                    output.motion.y = -output.motion.y;
                output.depth = SAMPLE_TEXTURE2D(_DlssFgInputDepth,
                    sampler_PointClamp, depthUv).r;
                return output;
            }
            ENDHLSL
        }

        Pass
        {
            Name "ValidateInputs"

            HLSLPROGRAM
            #pragma target 4.5
            #pragma vertex VertDebug
            #pragma fragment FragDebug

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

            TEXTURE2D(_DlssFgDebugDepth);
            TEXTURE2D(_DlssFgDebugMotion);
            float4x4 _DlssFgDebugClipToPrevClip;
            float4 _DlssFgDebugSize;   // width, height, 1/width, 1/height
            float4 _DlssFgDebugParams; // mode, depthInverted, motionRangePixels, near
            float4 _DlssFgDebugMotionScale; // exact MvecScale submitted to NGX

            struct DebugAttributes
            {
                uint vertexID : SV_VertexID;
            };

            struct DebugVaryings
            {
                float4 positionCS : SV_POSITION;
                float2 uv : TEXCOORD0;
            };

            DebugVaryings VertDebug(DebugAttributes input)
            {
                DebugVaryings output;
                output.positionCS = GetFullScreenTriangleVertexPosition(input.vertexID);
                output.uv = GetFullScreenTriangleTexCoord(input.vertexID);
                return output;
            }

            float2 CameraMotionPixels(float2 uv, float depth)
            {
                // NGX screen space is top-left. Convert that screen position to
                // D3D clip space, transform current clip to previous clip, then
                // convert the result back to top-left pixel coordinates.
                float2 currentNdc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
                float4 currentClip = float4(currentNdc, depth, 1.0);
                float4 previousClip = mul(_DlssFgDebugClipToPrevClip, currentClip);
                float2 previousNdc = previousClip.xy / max(abs(previousClip.w), 1e-8) *
                    sign(previousClip.w);
                float2 previousUv = float2(previousNdc.x * 0.5 + 0.5,
                    0.5 - previousNdc.y * 0.5);
                return (previousUv - uv) * _DlssFgDebugSize.xy;
            }

            float3 VisualizeMotion(float2 motionPixels)
            {
                float rangePixels = _DlssFgDebugParams.z;
                float2 direction = saturate(0.5 + motionPixels / (2.0 * rangePixels));
                return float3(direction, saturate(length(motionPixels) / rangePixels));
            }

            float4 FragDebug(DebugVaryings input) : SV_Target
            {
                float2 uv = input.uv;
                float depth = SAMPLE_TEXTURE2D(_DlssFgDebugDepth, sampler_PointClamp, uv).r;
                float2 urpMotion = SAMPLE_TEXTURE2D(_DlssFgDebugMotion,
                    sampler_PointClamp, uv).xy;
                float2 actualMotionPixels = urpMotion * _DlssFgDebugMotionScale.xy;
                float2 cameraMotionPixels = CameraMotionPixels(uv, depth);
                int mode = (int)_DlssFgDebugParams.x;

                if (mode == 1)
                {
                    // Gamma expansion keeps small reversed-Z values visible.
                    return float4(pow(saturate(depth), 0.25).xxx, 1.0);
                }

                if (mode == 2)
                {
                    float eyeDepth = LinearEyeDepth(depth, _ZBufferParams);
                    float logDepth = log2(max(eyeDepth, _DlssFgDebugParams.w) /
                        _DlssFgDebugParams.w);
                    return float4(saturate(logDepth / 16.0).xxx, 1.0);
                }

                if (mode == 3)
                    return float4(VisualizeMotion(actualMotionPixels), 1.0);
                if (mode == 4)
                    return float4(VisualizeMotion(cameraMotionPixels), 1.0);

                bool invalidDepth = _DlssFgDebugParams.y > 0.5
                    ? depth <= 1e-7 : depth >= 1.0 - 1e-7;
                float errorPixels = length(actualMotionPixels - cameraMotionPixels);
                float good = 1.0 - smoothstep(0.25, 1.0, errorPixels);
                float bad = smoothstep(0.25, 1.0, errorPixels);
                float3 errorColor = float3(bad, good, 0.0);
                if (invalidDepth)
                    errorColor = float3(0.0, 0.0, 0.25);

                if (mode == 5)
                    return float4(errorColor, 1.0);

                // Left: submitted texture motion. Right: camera-matrix prediction.
                return uv.x < 0.5
                    ? float4(VisualizeMotion(actualMotionPixels), 1.0)
                    : float4(VisualizeMotion(cameraMotionPixels), 1.0);
            }
            ENDHLSL
        }
    }
    Fallback Off
}
