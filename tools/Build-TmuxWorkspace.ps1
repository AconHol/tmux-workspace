#Requires -Version 7
[CmdletBinding()]
param(
    [string] $OutputDirectory,
    [ValidateRange(1, 32)] [int] $Parallelism = 6
)
$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repo 'artifacts\TmuxWorkspace'
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio C++ tools were not found.' }
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\amd64\MSBuild.exe'
Push-Location -LiteralPath $repo
try {
    # Terminal-specific packages use Microsoft's official feed and the versions
    # pinned in the upstream repository.
    & .\dep\nuget\nuget.exe restore dep\nuget\packages.config -NonInteractive -Verbosity quiet
    if ($LASTEXITCODE) { throw 'Dependency restore failed.' }
    & .\dep\nuget\nuget.exe restore .nuget\packages.config -NonInteractive -Verbosity quiet
    if ($LASTEXITCODE) { throw 'Build tool restore failed.' }
    & $msbuild OpenConsole.slnx '/t:Terminal\Window\WindowsTerminal' "/m:$Parallelism" /p:Configuration=Release /p:Platform=x64 /nologo /verbosity:minimal
    if ($LASTEXITCODE) { throw 'Windows Terminal build failed.' }
    & $msbuild src\cascadia\WindowsTerminal\WindowsTerminal.vcxproj /t:_WTPrepareUnpackagedLayoutForRun /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$repo\" /p:BuildProjectReferences=false /nologo /verbosity:minimal
    if ($LASTEXITCODE) { throw 'Portable layout preparation failed.' }
    $layout = Join-Path $repo 'bin\x64\Release\WindowsTerminal'
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    Get-ChildItem -LiteralPath $layout -File | Where-Object Extension -In '.exe', '.dll', '.pri', '.json', '.winmd' |
        Copy-Item -Destination $OutputDirectory -Force
    Get-ChildItem -LiteralPath $layout -Directory | Where-Object Name -NotIn '_xaml', 'settings' |
        Copy-Item -Destination $OutputDirectory -Recurse -Force
    New-Item -ItemType File -Path (Join-Path $OutputDirectory '.portable') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $OutputDirectory -Force
    Copy-Item -LiteralPath (Join-Path $repo 'NOTICE.md') -Destination $OutputDirectory -Force
    Copy-Item -LiteralPath (Join-Path $repo 'TMUX-WORKSPACE.md') -Destination (Join-Path $OutputDirectory '使用说明.md') -Force
    Write-Host "Portable application: $OutputDirectory"
}
finally {
    Pop-Location
}
