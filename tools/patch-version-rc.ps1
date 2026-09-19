<#
.SYNOPSIS
  Stamps version.rc's FILEVERSION/PRODUCTVERSION for a local build.

.DESCRIPTION
  Both StarRupture-ModLoader-Core\version.rc and StarRupture-ModLoader-Proxy\
  version.rc ship checked in with the placeholder FILEVERSION/PRODUCTVERSION
  1,0,0,0 -- the real per-release number is stamped in by
  .github/workflows/release.yml's "Patch version.rc with release tag" step,
  which only ever runs in CI. A plain local MSBuild build never touches
  version.rc, so its Core DLL reads as file version 1.0.0.0, which the
  auto-updater (updater.cpp RunUpdater) treats as older than any real
  release and offers to replace on the very next launch.

  This script applies the same patch CI does -- same regexes, same
  comma/dot formats -- so a local build stamped with a real version number
  reads as up to date against that release, and as outdated against a newer
  one. See updater.cpp's ParseVersion/CompareVersions for the comparison
  this feeds.

  Called from Shared.props' PatchModLoaderVersionRc target when
  /p:ModLoaderVersion=X.Y.Z is passed to MSBuild; not invoked otherwise, so
  a plain build is unaffected. Not called by CI, which patches version.rc
  itself before MSBuild ever runs -- this script and that step just happen
  to do the same thing to the same files.

  A local build's version.rc is a tracked, checked-in file, so patching it
  in place would leave the working tree dirty after every build -- easy to
  commit by accident, and a rebase-blocker while it's sitting there. To
  keep it non-destructive, the patch step first copies each version.rc to
  a "*.modloader-orig" sidecar next to it (skipped if that sidecar already
  exists, so a second project's identical patch pass doesn't overwrite a
  genuine original with already-stamped content), and Shared.props' paired
  RestoreModLoaderVersionRc target runs this same script with -Restore
  immediately after ResourceCompile to move the sidecar back over
  version.rc and delete it -- so by the time the build finishes, only the
  compiled .res/.dll carry the stamped version; the source file is exactly
  what git already has.

.PARAMETER Version
  Release-style version, e.g. "1.21.2" or "v1.21.2" -- a leading "v" and
  any "-suffix"/"+build" are stripped, same as CI does to its tag. Padded
  with trailing zero parts if shorter than four (matching a bare FILEVERSION
  quad, e.g. "1.21" becomes 1,21,0,0). Ignored (and not required) with
  -Restore.

.PARAMETER SolutionDir
  Directory containing both projects' version.rc files. Defaults to this
  script's own repo root (tools\..\).

.PARAMETER Restore
  Reverses the patch: moves each version.rc's "*.modloader-orig" sidecar
  back over version.rc and removes the sidecar. A missing sidecar (already
  restored by the other project's pass, or the patch step never ran) is a
  silent no-op, not an error -- both targets fire once per project build,
  so every restore after the first is expected to find nothing to do.

.EXAMPLE
  .\tools\patch-version-rc.ps1 -Version 1.21.2
.EXAMPLE
  .\tools\patch-version-rc.ps1 -Restore
#>
param(
    [string] $Version,

    [string] $SolutionDir = (Split-Path -Parent (Split-Path -Parent $PSCommandPath)),

    [switch] $Restore
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $Restore -and -not $Version) {
    throw "patch-version-rc: -Version is required unless -Restore is passed."
}

$rcPaths = @(
    (Join-Path $SolutionDir 'StarRupture-ModLoader-Proxy\version.rc'),
    (Join-Path $SolutionDir 'StarRupture-ModLoader-Core\version.rc')
)

if ($Restore) {
    foreach ($rcPath in $rcPaths) {
        $backupPath = "$rcPath.modloader-orig"
        if (-not (Test-Path $backupPath)) {
            continue # already restored by the other project's pass, or never patched
        }
        Move-Item -LiteralPath $backupPath -Destination $rcPath -Force
        Write-Host "patch-version-rc: restored $rcPath"
    }
    return
}

$tag = $Version -replace '^v', '' -replace '[-+].*$', ''
$parts = $tag -split '\.'
while ($parts.Count -lt 4) { $parts += '0' }
$commas = $parts[0..3] -join ','
$dots   = $parts[0..3] -join '.'

foreach ($rcPath in $rcPaths) {
    if (-not (Test-Path $rcPath)) {
        Write-Warning "patch-version-rc: $rcPath not found -- skipped"
        continue
    }

    $backupPath = "$rcPath.modloader-orig"
    if (-not (Test-Path $backupPath)) {
        Copy-Item -LiteralPath $rcPath -Destination $backupPath
    }

    $rc = Get-Content $rcPath -Raw
    $rc = $rc -replace 'FILEVERSION\s+[\d,]+',    "FILEVERSION     $commas"
    $rc = $rc -replace 'PRODUCTVERSION\s+[\d,]+', "PRODUCTVERSION  $commas"
    $rc = $rc -replace '"FileVersion",\s+"[^"]*"',    "`"FileVersion`",      `"$dots`""
    $rc = $rc -replace '"ProductVersion",\s+"[^"]*"', "`"ProductVersion`",   `"$dots`""

    # CI's own step (a pwsh/PS7 runner) writes -Encoding UTF8NoBOM directly;
    # Set-Content's -Encoding parameter doesn't know that name on Windows
    # PowerShell 5.1, which is what runs here for a local build, so write the
    # bytes directly instead -- works identically on both.
    [System.IO.File]::WriteAllText($rcPath, $rc, (New-Object System.Text.UTF8Encoding $false))
    Write-Host "patch-version-rc: $rcPath -> $dots"
}
