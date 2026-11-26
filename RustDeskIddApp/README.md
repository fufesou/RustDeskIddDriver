# RustDeskIddDriver

This is a sample based on [Indirect Display Driver Sample](https://github.com/microsoft/Windows-driver-samples/tree/master/video/IndirectDisplay).

## Environment

Windows 10 version 1903 (build 18362) or later. SDR works with IddCx 1.4. HDR requires Windows with IddCx 1.10 and a driver built with HDR support.

## Steps

1. Extract the package and install its Microsoft Visual C++ Redistributable (`vc_redist.x64.exe` for x64). Both the controller and the driver host use the system runtime.
2. For a test-signed package, use a Windows driver test machine configured to load drivers signed by the supplied test certificate.
3. Run `RustDeskIddApp.exe` as administrator. Press `c` to create the software device and `i` to install or update its driver. Keep the controller open while using a handle-lifetime device. If installation reports `0xe0000247`, check the device-install section in `%SystemRoot%\inf\setupapi.dev.log` for the signing or staging error.

## Demo

If all are going right, you can see RustDeskIddDriver in your device manager.

## Display profiles

Create a profile once per monitor. Keep its generated `id` to reuse the Windows monitor identity after a disconnect. Connector numbers run from 0 to 9. Creating and validating a file work offline. To apply it, the installed driver and its software device must be active. When the interactive app creates a device with `c`, leave that window open and run the following commands in a second elevated terminal.

```powershell
.\RustDeskIddApp.exe profile init gaming.profile 0
.\RustDeskIddApp.exe profile validate gaming.profile
.\RustDeskIddApp.exe profile apply gaming.profile
```

Edit the UTF-8 file before applying it. For example, retain the generated `id` and `connector` lines and set the remaining fields as follows.

```ini
enabled=1
hdr=1
cursor=0
mode=3840x2160@120
mode=2560x1440@144
mode=1920x1080@240
mode=1920x1080@60000/1001
```

- The first mode is preferred. Profiles support integer and fractional refresh rates. Offline validation accepts dimensions up to 16,384 pixels per axis. The generated profile starts in SDR with a composited cursor.
- A generated EDID uses a 1.4 base block. The driver adjusts blanking to encode the requested refresh rate exactly. If the preferred timing does not fit, the base block uses the first encodable profile mode, or standard VGA when none fits. The CTA extension can identify the preferred mode by video code or a Type VII detailed timing. Modes outside these formats remain available through IddCx with their configured refresh rates. Windows stores pixel clocks in whole hertz and line rates as 32-bit fractions. Changing the preferred mode preserves the other modes’ timings. Setting `edid=monitor.bin` supplies the base and extension blocks directly.
- `hdr=1` exposes Windows HDR controls on supported systems. Enable **Use HDR** for the virtual monitor in Windows Display settings. `cursor=1` enables a separate hardware cursor. A streaming client must draw that cursor independently, including its visibility, hotspot and shape updates. If setup fails, video stays active and the cursor query reports the Windows error. Apply `cursor=0` to request a composited cursor.
- To supply a display descriptor, add `edid=monitor.bin`. Relative paths resolve from the profile directory. An empty or omitted `edid` uses the generated descriptor. The driver checks the header, extension count, checksums and timing-block structure.
- For progressive modes, an exact detailed timing in the base EDID, CTA, VTB-EXT or DisplayID Type I/VII has first priority. Next come CTA and HDMI 1.4 video codes, three-byte CVT codes, two-byte standard timings and established timings, in that order. Within each VESA timing class, the earlier record takes priority. When a CVT code prefers 60 Hz and advertises both blanking variants, 60 Hz selects reduced blanking. Distinct fractional refresh rates can select the corresponding variants. The driver then checks for a detailed timing within half its encoded pixel-clock step, and for integer requests it also accepts a unique detailed timing that rounds to that rate. The 60 Hz and 60000/1001 Hz broadcast modes remain distinct.
- For a non-DMT standard timing in the EDID 1.4 base block, the driver uses CVT when the range descriptor is absent or specifies limits only. Explicit default or secondary GTF selects GTF. If a formula cannot produce a usable timing, the controller and driver report the affected resolution, refresh rate and reason. A profile requesting that rate needs another usable EDID timing for it. Other modes remain available, including additional profile modes that use driver timings. Validation rejects two entries that resolve to the same scan timing.
- The driver publishes RGB modes. A mode listed in a CTA YCbCr 4:2:0-only video block requires an independently advertised RGB timing, which can include an EDID 1.4 three-byte CVT code with standard or reduced blanking. A CTA capability map that also permits 4:2:0 leaves the RGB timing available.
- A custom HDR descriptor must advertise PQ and HDR static metadata type 1 in a CTA extension or a CTA data block embedded in DisplayID.
- Reapply the file after changing it or recreating the software device. Changes to the mode list, its order or the monitor description can briefly disconnect the monitor. The controller locates an existing monitor by its connector and ID across adapters and reports duplicate matches. Use a unique ID per active monitor. Two monitors sharing identical EDID bytes must also share their monitor modes and HDR capability because Windows supplies only descriptor bytes to the mode parser.
- Set `enabled=0` and apply the file to disconnect its monitor. In the interactive menu, `a` tries the next connector in sequence and `A` lets you enter a connector number. For example, if connector 0 is occupied or disabled, press `A` and enter `1` to use connector 1. Both commands use `%LOCALAPPDATA%\RustDeskIddApp\connector-N.profile`. Use `m` to update a connector listed for this session and `b` to disconnect the last monitor this session added. Reconnecting the same monitor starts a new attachment.

## Render adapter and live output

```powershell
.\RustDeskIddApp.exe gpu list
.\RustDeskIddApp.exe adapter list
.\RustDeskIddApp.exe gpu set 0
.\RustDeskIddApp.exe gpu set auto
.\RustDeskIddApp.exe cursor 0
.\RustDeskIddApp.exe color 0
.\RustDeskIddApp.exe gpu set 0 --profile gaming.profile
.\RustDeskIddApp.exe cursor --profile gaming.profile
.\RustDeskIddApp.exe color --profile gaming.profile
.\RustDeskIddApp.exe modes gaming.profile
.\RustDeskIddApp.exe hdr-capture gaming.profile capture.png
```

`modes PROFILE` uses the profile's connector and monitor ID to find its active Windows display target. It lists the resolutions and integer refresh rates Windows exposes for that target's desktop source. `gpu set` supplies an adapter-wide preference to Windows. Choose an index from the current `gpu list`, or use `auto` to clear the preference. Windows assigns the actual render adapter.

With one virtual adapter, `gpu set INDEX|auto` selects it directly. With multiple adapters, add `--profile FILE` to select the adapter that owns that active monitor. `cursor N` and `color N` query the adapter with an active monitor on connector N. If multiple adapters use that connector, specify `--profile FILE` in place of N. These commands only need the connector and monitor ID from the file. `color` reports the frame surface color space, the committed output color space and RGB bits per component. Capture applications can also retrieve cursor pixels, SDR white level, HDR10 metadata and the Windows color transform from the control interface.

To select a virtual adapter before its first monitor connects, copy its device instance ID from `adapter list`. Pass it to `gpu set INDEX --adapter "INSTANCE_ID"` and `profile apply FILE --adapter "INSTANCE_ID"`. Without an explicit target, a new profile tries the adapters with a free connector until one can supply a configured mode. If another request takes the connector, the controller checks ownership again. Reapplying an active profile uses its existing adapter.

The legacy mode-update IOCTL updates the connected monitor in place. Include the resolutions and refresh rates you want to switch between in the monitor's profile. At least one requested target timing must match an advertised monitor mode and fit the current render GPU. A rejected update preserves the previous list. Changing the advertised modes through the profile can reconnect the monitor so Windows reads the new description.

The target mode list contains the configured modes that fit the rendering GPU, in profile order. D3D feature levels 10.x, 9.3 and 9.1/9.2 allow 8,192, 4,096 and 2,048 pixels per axis respectively. Before Windows assigns a GPU, automatic selection uses the smallest limit among adapters whose D3D11 probes succeed. The driver logs probe errors for failed render adapters. If all probes fail, it returns an error. With `gpu set INDEX`, the driver checks the selected adapter directly. Applying a profile or selecting a GPU fails if it leaves a monitor with no usable mode. If Windows independently moves an active monitor to such a GPU, the driver withdraws that monitor. Reapply its profile with a supported mode or GPU to reconnect it.

If a replacement profile fails to connect, the driver tries to restore the previous monitor and its published modes. It rechecks the current preference and GPUs previously assigned or selected for that monitor. If their capabilities have changed, it restores the largest subset of published modes that a usable GPU can render. If no mode fits, the monitor stays disconnected and the request reports the restoration error.

`modes` and `hdr-capture` read live Windows or driver state. They can use the profile after its external EDID file has been moved. Applying or validating an enabled profile still reads its referenced EDID.

With Windows HDR enabled, `hdr-capture PROFILE NEW.png` captures one frame from the named monitor in the interactive Windows session. It reads calibration from the adapter that owns the profile connector and container ID and reports an error if multiple adapters match. The capture device uses the render adapter Windows assigned to the monitor's swap chain. It reads FP16 scRGB pixels, applies the active Windows calibration in CIE XYZ and the lookup table in BT.2020/PQ, then saves a full-range 10-bit HDR image in a 16-bit PNG. The PNG includes its color space and any supplied HDR10 mastering and light-level metadata. Use a stable scene when checking calibration. The command reports a color-state change during acquisition and refuses to overwrite an existing file. Screen capture runs only on request. External streaming applications must apply the calibration and attach metadata to their own frames.

To check SDR capture, display known sRGB patches on an `hdr=0` monitor and capture them with the streaming application. Compare the captured values against the expected output with identity calibration and with a known nonidentity calibration. Repeat with HDR enabled on a second monitor.

## View logs

You need a TraceView.exe(contained in WDK).

```
Launch TraceView.exe
        |
        v
       File
        |
        v
Create New Log Session
        |
        v
   Add Provider
        |
        v
    Select PDB Path
        |
        v
Ok --> Next --> Finish
```
