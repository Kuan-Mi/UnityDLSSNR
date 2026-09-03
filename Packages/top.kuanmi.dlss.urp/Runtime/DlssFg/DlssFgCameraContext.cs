using System;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.Rendering;

namespace UnityRhi.Dlss.Urp
{
    /// <summary>
    /// Persistent depth/motion copies whose D3D12 resources the native Present
    /// path can retain across Evaluate.
    /// </summary>
    internal sealed class DlssFgCameraContext : IDisposable
    {
        internal int Width { get; }
        internal int Height { get; }
        internal RenderTexture MotionRt { get; private set; }
        internal RenderTexture DepthRt { get; private set; }
        internal RTHandle MotionHandle { get; private set; }
        internal RTHandle DepthHandle { get; private set; }

        private Matrix4x4 _prevViewProj = Matrix4x4.identity;
        private int _lastFrame = int.MinValue;
        private Vector3 _lastPosition;
        private Quaternion _lastRotation;
        private Matrix4x4 _lastProjection;
        private bool _hasHistory;
        private bool _disposed;

        internal DlssFgCameraContext(int width, int height, string cameraName)
        {
            if (width <= 0) throw new ArgumentOutOfRangeException(nameof(width));
            if (height <= 0) throw new ArgumentOutOfRangeException(nameof(height));
            Width = width;
            Height = height;

            try
            {
                MotionRt = CreateRenderTexture($"DLSS-G {cameraName} Motion", width, height,
                    GraphicsFormat.R16G16_SFloat);
                DepthRt = CreateRenderTexture($"DLSS-G {cameraName} Depth", width, height,
                    GraphicsFormat.R32_SFloat);
                MotionHandle = RTHandles.Alloc(MotionRt);
                DepthHandle = RTHandles.Alloc(DepthRt);
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        internal void BeginFrame(int frameIndex, Vector3 position, Quaternion rotation,
            in Matrix4x4 projection, in Matrix4x4 viewProj, in DlssFgSettings settings,
            out Matrix4x4 prevViewProj, out bool reset)
        {
            reset = !_hasHistory || frameIndex != _lastFrame + 1;
            if (_hasHistory)
            {
                if (Vector3.Distance(_lastPosition, position) > settings.CameraCutDistance ||
                    Quaternion.Angle(_lastRotation, rotation) > settings.CameraCutAngle ||
                    ProjectionChanged(_lastProjection, projection))
                    reset = true;
            }

            prevViewProj = _hasHistory ? _prevViewProj : viewProj;
            _prevViewProj = viewProj;
            _lastFrame = frameIndex;
            _lastPosition = position;
            _lastRotation = rotation;
            _lastProjection = projection;
            _hasHistory = true;
        }

        internal void ResetHistory() => _hasHistory = false;

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            DepthHandle?.Release(); DepthHandle = null;
            MotionHandle?.Release(); MotionHandle = null;
            Destroy(DepthRt); DepthRt = null;
            Destroy(MotionRt); MotionRt = null;
        }

        private static RenderTexture CreateRenderTexture(string name, int width, int height,
            GraphicsFormat format)
        {
            var rt = new RenderTexture(new RenderTextureDescriptor(width, height)
            {
                graphicsFormat = format,
                depthStencilFormat = GraphicsFormat.None,
                msaaSamples = 1,
                volumeDepth = 1,
                dimension = UnityEngine.Rendering.TextureDimension.Tex2D,
                enableRandomWrite = false,
                sRGB = false,
                useMipMap = false,
            })
            {
                name = name,
                filterMode = FilterMode.Point,
                wrapMode = TextureWrapMode.Clamp,
                hideFlags = HideFlags.HideAndDontSave,
            };
            if (!rt.Create())
                throw new InvalidOperationException($"Could not create '{name}'.");
            return rt;
        }

        private static bool ProjectionChanged(in Matrix4x4 a, in Matrix4x4 b)
        {
            for (int i = 0; i < 16; ++i)
                if (Mathf.Abs(a[i] - b[i]) > 1e-4f)
                    return true;
            return false;
        }

        private static void Destroy(RenderTexture texture)
        {
            if (texture == null) return;
            texture.Release();
            if (Application.isPlaying)
                UnityEngine.Object.Destroy(texture);
            else
                UnityEngine.Object.DestroyImmediate(texture);
        }
    }
}
