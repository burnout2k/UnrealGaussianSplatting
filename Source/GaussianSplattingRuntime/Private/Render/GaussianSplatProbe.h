#pragma once

// Fix 5 Step 0 probe (splat-work/fix5-plan.md v2, "Build A"): measurements for the streaming pool before any streaming
// code exists. Every probe is off unless one of the r.GaussianSplat.Probe.* console variables is set.
namespace GaussianSplatProbe
{
    // Registers a tick function in TG_LastDemotable on every game world: after every camera pose is final and
    // before CARLA's captures, where Fix 5's settle gate will run.
    void Startup();
    void Shutdown();
}
