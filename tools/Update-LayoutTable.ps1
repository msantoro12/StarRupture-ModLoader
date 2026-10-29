<#
.SYNOPSIS
    Regenerates StarRupture-ModLoader-Core\core\layout_table.generated.h from
    the Game-SDK submodule.

.DESCRIPTION
    The layout table is the list of SDK classes the loader uses, with each
    class's size and each member's offset, size and bool mask as the SDK has
    them. At startup the loader compares it with the running game's reflection
    data (core\layout_check.cpp) and logs every class whose layout moved.

    The table must match the SDK the loader is built against, so run this
    after every "Update SDK.bat" and commit the result together with the
    submodule bump. The generator is layout-table.mjs from the starrupture-ci
    repo (https://github.com/msantoro12/starrupture-ci, ci\layout-table.mjs);
    it needs Node.js. Its format and the checks the loader makes are described
    in ci\LAYOUT-TABLE.md there.

    The classes come from the loader's own sources (every SDK type they name)
    plus their reflected base classes. The built CL is read from
    kRequiredVersionSuffix in core\version_check.cpp.

.PARAMETER Generator
    Path to layout-table.mjs.

.EXAMPLE
    .\tools\Update-LayoutTable.ps1 -Generator ..\starrupture-ci\ci\layout-table.mjs
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Generator
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $root 'StarRupture SDK\Client'
$out = Join-Path $root 'StarRupture-ModLoader-Core\core\layout_table.generated.h'

if (-not (Test-Path $Generator)) { throw "Generator not found: $Generator" }
if (-not (Test-Path (Join-Path $sdk 'Assertions.inl'))) {
    throw "No Client\Assertions.inl under '$sdk'. Run: git submodule update --init `"StarRupture SDK`""
}

$versionCheck = Get-Content -Raw (Join-Path $root 'StarRupture-ModLoader-Core\core\version_check.cpp')
if ($versionCheck -notmatch 'kRequiredVersionSuffix\[\]\s*=\s*L"CL-(\d+)"') {
    throw 'Could not read the CL from kRequiredVersionSuffix in core\version_check.cpp'
}
$cl = $Matches[1]

& node $Generator gen (Join-Path $sdk 'Assertions.inl') --cl $cl `
    --src $root --sdk (Join-Path $sdk 'SDK') --out $out
if ($LASTEXITCODE -ne 0) { throw "layout-table.mjs failed (exit $LASTEXITCODE)" }

Write-Host "Wrote $out for CL $cl"
