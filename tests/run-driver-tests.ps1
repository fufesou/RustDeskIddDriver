param(
    [ValidateSet(4, 10)] [int] $IddMinor = 10,
    [string] $Kit = $env:WindowsSdkDir,
    [string] $SdkVersion = $env:WindowsSDKVersion,
    [string] $IddInclude,
    [switch] $BuildOnly
)
$ErrorActionPreference = 'Stop'
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
if (!$Kit -or !$SdkVersion) { throw 'Run in a Visual Studio Native Tools prompt.' }
$SdkVersion = $SdkVersion.TrimEnd('\')
if (!$IddInclude) { $IddInclude = Join-Path $Kit "Include/$SdkVersion/um/iddcx/1.$IddMinor" }
$project = Join-Path $PSScriptRoot '../RustDeskIddDriver'
$build = Join-Path $PSScriptRoot "build/$env:VSCMD_ARG_TGT_ARCH/iddcx-1.$IddMinor"
[void][IO.Directory]::CreateDirectory($build)
Push-Location $build
try {
    $traced = 'Driver.cpp','DeviceControl.cpp','MonitorDevice.cpp','HardwareCursor.cpp','SwapChain.cpp' |
        ForEach-Object { Join-Path $project $_ }
    $tools = Join-Path $Kit "bin/$SdkVersion"
    & "$tools/$env:VSCMD_ARG_HOST_ARCH/tracewpp.exe" -um "-cfgdir:$tools/WppConfig/Rev1" "-scan:$project/Trace.h" "-odir:$build" @traced
    if ($LASTEXITCODE) { throw "Generating driver traces failed ($LASTEXITCODE)" }
    $callingConvention = if ($env:VSCMD_ARG_TGT_ARCH -eq 'x86') { '/Gz' } else { '/Gd' }
    & $compiler /nologo /EHsc /std:c++17 /MD /Gy $callingConvention /DUMDF_DRIVER /DUMDF_VERSION_MAJOR=2 /DUMDF_VERSION_MINOR=25 `
        /DIDDCX_VERSION_MAJOR=1 "/DIDDCX_VERSION_MINOR=$IddMinor" /DIDDCX_MINIMUM_VERSION_REQUIRED=4 `
        /DWPP_MACRO_USE_KM_VERSION_FOR_UM "/I$IddInclude" "/I$Kit/Include/wdf/umdf/2.25" "/I$build" `
        "$PSScriptRoot/driver_profiles.cpp" "$PSScriptRoot/driver_device.cpp" "$project/../RustDeskIddApp/HdrColor.cpp" `
        "$project/MonitorProfile.cpp" "$project/GeneratedEdid.cpp" "$project/ColorState.cpp" `
        "$project/CursorState.cpp" "$project/AdvancedColor.cpp" "$project/DisplayModes.cpp" @traced `
        /link /OPT:REF uuid.lib advapi32.lib avrt.lib dxgi.lib d3d11.lib /OUT:driver_profiles.exe
    if ($LASTEXITCODE) { throw "Compiling driver tests failed ($LASTEXITCODE)" }
    if ($BuildOnly) { return }
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $build 'driver_profiles.exe'))
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $process = [Diagnostics.Process]::Start($start)
    if (!$process.WaitForExit(60000)) {
        $process.Kill()
        throw 'Driver tests exceeded 60 seconds'
    }
    if ($process.ExitCode) { throw "Driver tests failed ($($process.ExitCode))" }
    Write-Host "IddCx 1.$IddMinor driver tests passed"
} finally {
    Pop-Location
}
