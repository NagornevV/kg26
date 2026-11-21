$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2022 with Desktop development with C++.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio C++ toolset was not found.' }
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
Push-Location $repo
try {
    & $msbuild tests\TerrainChecks.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Terrain checks build failed.' }
    & .\x64\TerrainChecks\TerrainChecks.exe terrain/heightmap.png terrain/tiles
    if ($LASTEXITCODE -ne 0) { throw 'Terrain checks failed.' }
} finally { Pop-Location }
