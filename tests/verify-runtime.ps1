param(
    [Parameter(Mandatory)] [string] $Runtime,
    [Parameter(Mandatory)] [version] $CompilerVersion
)
$ErrorActionPreference = 'Stop'
$file = Get-Item -LiteralPath $Runtime
$signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(^|, )O=Microsoft Corporation(,|$)') {
    throw "Microsoft redistributable signature validation failed for $($file.Name): $($signature.Status)"
}
if ($file.VersionInfo.OriginalFilename -ne $file.Name) {
    throw "Redistributable architecture or filename mismatch: $($file.VersionInfo.OriginalFilename)"
}
$version = [version]$file.VersionInfo.ProductVersion
if ($version -lt $CompilerVersion) {
    throw "Redistributable $version is older than the compiler toolset $CompilerVersion"
}
Write-Host "$($file.Name) is Microsoft-signed, version $version"
