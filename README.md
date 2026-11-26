# RustDesk indirect display driver

RustDeskIddDriver creates up to ten virtual monitors on Windows. Each connector can have its own persistent identity, preferred mode, rational refresh rates, EDID, HDR setting and hardware cursor setting. The controller can also suggest a render GPU for the adapter. The original plug, unplug and mode-update requests remain available to existing clients. [Controller commands](RustDeskIddApp/README.md) · [Control interface](RustDeskIddDriver/Public.h)

## Build and test

Build the solution with Visual Studio, the C++ desktop tools, and a matching Windows SDK and WDK. The x64 and ARM64 driver configurations target IddCx 1.10 with a 1.4 runtime minimum. WDK 10.0.26100 includes the 1.10 headers and libraries. To build with an older WDK that contains IddCx 1.4, add `/p:IDDCX_VERSION_MINOR=4` to the driver build. HDR requires a 1.10 build and runtime. An HDR profile on an older runtime returns an unsupported-feature error.

From an x64 Visual Studio developer PowerShell with the SDK and WDK environment loaded, run the offline regression tests after building the Release app.

```powershell
python tests/profile_cli.py RustDeskIddApp/x64/Release/RustDeskIddApp.exe -v
./tests/run-driver-tests.ps1
```

For manual testing, install the package and create a profile on a free connector using the [controller commands](RustDeskIddApp/README.md).

- Start a fresh menu session with connectors 0 and 1 free and press `a` twice. In a second terminal, apply a copy of `connector-0.profile` with `enabled=0`. Press `A` and enter `0` to reconnect it. Verify that connector 1 stays online and that pressing `b` twice disconnects 0 first, then 1.
- On Windows 10 1903 or 22H2, apply an SDR profile and compare `modes PROFILE` with Windows Advanced display settings. Change the preferred refresh rate, test sleep and resume, then apply `enabled=0` and verify that the monitor disappears.
- On a supported Windows 11 system, apply `hdr=1`, enable **Use HDR** on the virtual monitor and display HDR content there. Check `color --profile PROFILE` and capture a frame with `hdr-capture PROFILE NEW.png`.
- With `cursor=1`, move the pointer over the virtual monitor and query `cursor --profile PROFILE`. Check that its position changes. In a capture client that supports hardware cursors, verify that exactly one pointer appears with both `cursor=1` and `cursor=0`.

Windows supplies the driver swap-chain surface on the assigned render adapter. Capture and encoding run in the streaming application. It must support HDR capture, color conversion and a 10-bit stream for HDR playback. It can read per-monitor color metadata and cursor updates through the [display profile interface](RustDeskIddDriver/DisplayProfile.h).

Adapted from Microsoft's [Indirect Display Driver Sample](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay).

<details>
<summary>Original Microsoft sample notes</summary>

# Indirect Display Driver Sample #

This is a sample driver that shows how to create a Windows Indirect Display Driver using IddCx (Indirect Display Driver Class eXtension).

## Background reading ##

Start at the [Indirect Display Driver Model Overview](https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx) on MSDN.

## Customizing the sample ##

The sample driver code is very simplistic and does nothing more than enumerate a single monitor when its device enters the D0/started power state. Throughout the code, there are `TODO` blocks with important information on implementing functionality in a production driver.

### Code structure ###

* `Direct3DDevice` class
    * Contains logic for enumerating the correct render GPU from DXGI and creating a D3D device.
    * Manages the lifetime of a DXGI factory and a D3D device created for the render GPU the system is using to render frames for your indirect display device's swap-chain.
* `SwapChainProcessor` class
    * Processes frames for a swap-chain assigned to the monitor object on a dedicated thread.
    * The sample code does nothing with the frames, but demonstrates a correct processing loop with error handling and notifying the OS of frame completion.
* `IndirectDeviceContext` class
    * Processes device callbacks from IddCx.
    * Manages the creation and arrival of the sample monitor.
    * Handles swap-chain arrival and departure by creating a `Direct3DDevice` and handing it off to a `SwapChainProcessor`.

### First steps ###

Consider the capabilities of your device. If the device supports multiple monitors being hotplugged and removed at runtime, you may want to abstract the monitors further from the `IndirectDeviceContext` class.

The INF file included in the sample needs updating for production use. One field, `DeviceGroupId`, controls how the UMDF driver gets pooled with other UMDF drivers in the same process. Since indirect display drivers tend to be more complicated than other driver classes, it's highly recommended that you pick a unique string for this field which will cause instances of your device driver to pool in a dedicated process. This will improve system reliability in case your driver encounters a problem since other drivers will not be affected.

Ensure the device information reported to `IddCxAdapterInitAsync` is accurate. This information determines how the device is reported to the OS and what static features (like support for gamma tables) the device will have available. If some information cannot be known immediately in the `EvtDeviceD0Entry` callback, IddCx allows the driver to call `IddCxAdapterInitAsync` at any point after D0 entry, before D0 exit.

Careful attention should be paid to the frame processing loop. This will directly impact the performance of the user's system, so making use of the [Multimedia Class Scheduler Service](https://msdn.microsoft.com/en-us/library/windows/desktop/ms684247(v=vs.85).aspx) and DXGI's support for [GPU prioritization](https://msdn.microsoft.com/en-us/library/windows/desktop/bb174534(v=vs.85).aspx) should be considered. Any significant work should be performed outside the main processing loop, such as by queuing work in a thread pool. See `SwapChainProcessor::RunCore` for more information.

</details>
