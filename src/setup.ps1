param(
    [Parameter(Mandatory = $true)]
    [string] $ArchivePath,
    [Parameter(Mandatory = $true)]
    [string] $Destination
)

$ErrorActionPreference = 'Stop'
$version = '26.11.1'
$downloadUrl = 'https://github.com/i-saint/usdTerrain/releases/download/usd-binaries/usd-26.11.1.7z'
$markerPath = Join-Path $Destination '.usdTerrain-sdk-version'
$requiredFiles = @(
    (Join-Path $Destination 'include\pxr\pxr.h'),
    (Join-Path $Destination 'lib\usd_ms.lib'),
    (Join-Path $Destination 'lib\usd_ms.dll')
)

if ((Test-Path $markerPath) -and ((Get-Content $markerPath -Raw).Trim() -eq $version) -and
    ($requiredFiles | Where-Object { -not (Test-Path $_) }).Count -eq 0) {
    Write-Output "USD $version is already installed at $Destination"
    exit 0
}

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'
$archiveDirectory = Split-Path -Parent $ArchivePath
$temporaryDirectory = "$Destination.extracting"
New-Item -ItemType Directory -Force -Path $archiveDirectory | Out-Null

if (-not (Test-Path $ArchivePath)) {
    Write-Output "Downloading USD $version"
    Invoke-WebRequest -Uri $downloadUrl -OutFile $ArchivePath
}

if (Test-Path $temporaryDirectory) {
    Remove-Item $temporaryDirectory -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null

& tar.exe -xf $ArchivePath -C $temporaryDirectory
if ($LASTEXITCODE -ne 0) {
    throw "Failed to extract USD archive (tar exit code $LASTEXITCODE)."
}

$sourceDirectory = $temporaryDirectory
if (-not (Test-Path (Join-Path $sourceDirectory 'include\pxr\pxr.h'))) {
    $nestedDirectory = Get-ChildItem $temporaryDirectory -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'include\pxr\pxr.h') } |
        Select-Object -First 1
    if ($nestedDirectory) {
        $sourceDirectory = $nestedDirectory.FullName
    }
}

$sourceRequiredFiles = @(
    (Join-Path $sourceDirectory 'include\pxr\pxr.h'),
    (Join-Path $sourceDirectory 'lib\usd_ms.lib'),
    (Join-Path $sourceDirectory 'lib\usd_ms.dll')
)
if (($sourceRequiredFiles | Where-Object { -not (Test-Path $_) }).Count -ne 0) {
    Remove-Item $temporaryDirectory -Recurse -Force
    throw 'The downloaded archive does not contain the expected USD SDK layout.'
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
if (Test-Path $Destination) {
    Remove-Item $Destination -Recurse -Force
}
Move-Item -LiteralPath $sourceDirectory -Destination $Destination
if (Test-Path $temporaryDirectory) {
    Remove-Item $temporaryDirectory -Recurse -Force
}
Set-Content -Path $markerPath -Value $version -Encoding ASCII
Remove-Item $ArchivePath -Force
Write-Output "Installed USD $version to $Destination"
