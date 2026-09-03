using System;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityRhi.Interop;

namespace UnityRhi
{
    /// <summary>
    /// Row-major, post-multiply (v' = v * M) 4x4 matching
    /// <c>NVSDK_NGX_DLSSG_Opt_Eval_Params</c>. Unity's column-major
    /// <see cref="Matrix4x4"/> memory layout is already that transform.
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct FrameGenerationMatrix
    {
        public Vector4 Column0;
        public Vector4 Column1;
        public Vector4 Column2;
        public Vector4 Column3;

        public static FrameGenerationMatrix FromUnity(in Matrix4x4 matrix) => new FrameGenerationMatrix
        {
            Column0 = matrix.GetColumn(0),
            Column1 = matrix.GetColumn(1),
            Column2 = matrix.GetColumn(2),
            Column3 = matrix.GetColumn(3),
        };
    }

    /// <summary>
    /// Blittable DLSS-G input packet. Native code copies it synchronously and
    /// retains <see cref="Depth"/> / <see cref="MotionVectors"/>.
    /// Resource states are raw D3D12 values, not NVRHI <see cref="ResourceStates"/>.
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 8, Size = 400)]
    public struct FrameGenerationInputs
    {
        public const uint D3D12ResourceStateCommon = 0;
        public const uint D3D12ResourceStateRenderTarget = 0x4;

        public IntPtr Depth;
        public IntPtr MotionVectors;
        public uint RenderWidth;
        public uint RenderHeight;
        public uint ColorWidth;
        public uint ColorHeight;
        public uint DepthState;
        public uint MotionVectorsState;
        public ulong FrameId;
        public FrameGenerationMatrix CameraViewToClip;
        public FrameGenerationMatrix ClipToCameraView;
        public FrameGenerationMatrix ClipToPrevClip;
        public FrameGenerationMatrix PrevClipToClip;
        public Vector3 CameraPos;
        public Vector3 CameraUp;
        public Vector3 CameraRight;
        public Vector3 CameraFwd;
        public float JitterX;
        public float JitterY;
        public float MotionVectorScaleX;
        public float MotionVectorScaleY;
        public float CameraNear;
        public float CameraFar;
        public float CameraFov;
        public float CameraAspect;
        public uint DepthInverted;
        public uint CameraMotionIncluded;
        public uint Reset;
        public uint ColorBuffersHdr;

        static FrameGenerationInputs()
        {
            int size = Marshal.SizeOf<FrameGenerationInputs>();
            int frameId = Marshal.OffsetOf<FrameGenerationInputs>(nameof(FrameId)).ToInt32();
            int cameraPos = Marshal.OffsetOf<FrameGenerationInputs>(nameof(CameraPos)).ToInt32();
            int hdr = Marshal.OffsetOf<FrameGenerationInputs>(nameof(ColorBuffersHdr)).ToInt32();
            if (size != 400 || frameId != 40 || cameraPos != 304 || hdr != 396)
                throw new InvalidOperationException(
                    $"FrameGenerationInputs ABI mismatch (size={size}, FrameId={frameId}, " +
                    $"CameraPos={cameraPos}, ColorBuffersHdr={hdr}).");
        }

        /// <summary>
        /// Copies this packet to the native Present path. The caller may release
        /// the managed copy immediately; native code retains the two D3D12 resources.
        /// </summary>
        public static unsafe void Submit(in FrameGenerationInputs inputs)
        {
            if (inputs.Depth == IntPtr.Zero || inputs.MotionVectors == IntPtr.Zero)
                return;
            FrameGenerationInputs copy = inputs;
            NativeMethods.UnityRhiSubmitFrameGenerationInputs(new IntPtr(&copy));
        }
    }
}
