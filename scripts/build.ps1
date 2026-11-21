param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2022 with Desktop development with C++.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio C++ toolset was not found.' }
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
Push-Location $repo
try {
    & $msbuild Laba_kg.sln /t:Restore /p:RestorePackagesConfig=true /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'NuGet restore failed.' }
    & $msbuild Laba_kg.sln /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    Write-Host "Built: $repo\x64\$Configuration\Laba_kg.exe"
} finally { Pop-Location }
