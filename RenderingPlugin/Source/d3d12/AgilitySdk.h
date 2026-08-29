#pragma once

// Opts the host process into the D3D12 Agility SDK when the host executable
// does not do it itself. See AgilitySdk.cpp for why this exists and when it
// does nothing.

namespace unityrhi
{
/// Must run before anything in the process creates a D3D12 device, which means
/// it must be called from UnityPluginLoad with the plugin marked "Load on
/// startup" (isPreloaded: 1). Safe and cheap to call more than once.
void EnableAgilitySdkIfHostDoesNot();

/// Absolute path of the D3D12Core.dll the process ended up with, or an empty
/// string when the module is not loaded. Only meaningful after the graphics
/// device exists; used to report which runtime actually won.
const char* ResolvedD3D12CorePath();
} // namespace unityrhi
