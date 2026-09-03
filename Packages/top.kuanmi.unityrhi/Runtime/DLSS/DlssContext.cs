using System;

namespace UnityRhi
{
    /// <summary>
    /// DLSS SuperSampling network preset passed to NGX. Letter values match
    /// NVIDIA's preset identifiers; 0 leaves the driver default.
    /// </summary>
    public enum DlssPreset : byte
    {
        Default = 0,
        PresetJ = 10,
        PresetK = 11,
    }

    /// <summary>Typed inputs for one DLSS SuperSampling dispatch.</summary>
    public sealed class DlssDispatchDesc
    {
        public Texture Input;
        public Texture Output;
        public Texture MotionVectors;
        public Texture Depth;
        public UnityEngine.Vector2 CameraJitterPixels;
        public int RenderWidth;
        public int RenderHeight;
        public int OutputWidth;
        public int OutputHeight;
        public float MotionVectorScaleX = 1f;
        public float MotionVectorScaleY = 1f;
        public UpscalerMode Mode = UpscalerMode.NATIVE;
        public DlssPreset Preset;
        public bool Reset;
        public bool DepthInverted = true;
    }

    /// <summary>
    /// Owns a native NGX SuperSampling (DLSS / DLAA) feature instance.
    /// Availability means NGX initialized and SuperSampling is present on this GPU.
    /// </summary>
    public sealed class DlssContext : IDisposable
    {
        private readonly int _instanceId;
        private bool _disposed;

        public DlssContext()
        {
            _instanceId = Interop.NativeMethods.UnityRhiCreateDlssInstance();
            if (_instanceId <= 0)
                throw new InvalidOperationException(
                    "UnityRHI could not create a DLSS SuperSampling context. " +
                    "Check RhiCore.IsNgxDlssAvailable and RhiCore.NgxDlssInitResult.");
        }

        public void Record(CommandList commandList, DlssDispatchDesc desc)
        {
            if (_disposed) throw new ObjectDisposedException(nameof(DlssContext));
            if (commandList == null) throw new ArgumentNullException(nameof(commandList));
            if (desc == null) throw new ArgumentNullException(nameof(desc));
            if (ReferenceEquals(desc.Input, desc.Output))
                throw new ArgumentException("DLSS color and output textures must be distinct.", nameof(desc));

            var command = new CommandWire.DlssDispatchPayload
            {
                Input = Handle(desc.Input, nameof(desc.Input)),
                Output = Handle(desc.Output, nameof(desc.Output)),
                MotionVectors = Handle(desc.MotionVectors, nameof(desc.MotionVectors)),
                Depth = Handle(desc.Depth, nameof(desc.Depth)),
                OutputWidth = Dimension(desc.OutputWidth, nameof(desc.OutputWidth)),
                OutputHeight = Dimension(desc.OutputHeight, nameof(desc.OutputHeight)),
                CurrentWidth = Dimension(desc.RenderWidth, nameof(desc.RenderWidth)),
                CurrentHeight = Dimension(desc.RenderHeight, nameof(desc.RenderHeight)),
                CameraJitterX = desc.CameraJitterPixels.x,
                CameraJitterY = desc.CameraJitterPixels.y,
                MotionVectorScaleX = desc.MotionVectorScaleX,
                MotionVectorScaleY = desc.MotionVectorScaleY,
                InstanceId = _instanceId,
                Reset = Flag(desc.Reset),
                DepthInverted = Flag(desc.DepthInverted),
                UpscalerMode = (byte)desc.Mode,
                Preset = (byte)desc.Preset,
            };
            commandList.DispatchDlss(command, desc.Input, desc.Output,
                desc.MotionVectors, desc.Depth);
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            Interop.NativeMethods.UnityRhiDestroyDlssInstance(_instanceId);
        }

        private static byte Flag(bool value) => value ? (byte)1 : (byte)0;

        private static ushort Dimension(int value, string name)
        {
            if (value <= 0 || value > ushort.MaxValue)
                throw new ArgumentOutOfRangeException(name, value,
                    "DLSS dimensions must be in the range 1..65535.");
            return checked((ushort)value);
        }

        private static ulong Handle(Texture texture, string name)
        {
            if (texture == null || !texture.IsValid)
                throw new ArgumentException("DLSS textures must be valid UnityRHI textures.", name);
            return texture.OpaqueIdentity;
        }
    }
}
