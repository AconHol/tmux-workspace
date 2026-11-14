#Requires -Version 7
[CmdletBinding()]
param(
    [string] $OutputDirectory,
    [Parameter(Mandatory)] [string] $CertificateThumbprint,
    [switch] $SkipBuild
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'artifacts\installer' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio C++ tools were not found.' }
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\amd64\MSBuild.exe'
$certificate = Get-Item -LiteralPath ("Cert:\CurrentUser\My\" + $CertificateThumbprint)
if (-not $certificate.HasPrivateKey) { throw 'The selected certificate has no usable private key in the local certificate store.' }
[xml]$manifest = Get-Content -LiteralPath (Join-Path $repo 'src\cascadia\CascadiaPackage\TmuxWorkspace.appxmanifest')
if ($manifest.Package.Identity.Publisher -cne $certificate.Subject) { throw 'MSIX publisher does not match the signing certificate.' }
$version = [string]$manifest.Package.Identity.Version
$sdk = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Windows Kits\10\bin\10.0.26100.0\x64'
$signtool = Join-Path $sdk 'signtool.exe'
Push-Location -LiteralPath $repo
try {
    if (-not $SkipBuild) {
        & .\dep\nuget\nuget.exe restore dep\nuget\packages.config -NonInteractive -Verbosity quiet
        if ($LASTEXITCODE) { throw 'Dependency restore failed.' }
        & $msbuild OpenConsole.slnx '/t:Terminal\Window\WindowsTerminal' /m:6 /p:Configuration=Release /p:Platform=x64 /p:TmuxWorkflowTests=false /nologo /verbosity:minimal
        if ($LASTEXITCODE) { throw 'Application build failed.' }
    }
    $payload = Join-Path $repo 'bin\x64\Release\WindowsTerminal\TerminalApp.dll'
    if ([Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($payload)).Contains('TMUX_WORKFLOW_REPORT')) {
        throw 'The workflow test driver must be excluded from the installed release. Rebuild with TmuxWorkflowTests=false.'
    }
    $buildReferences = if ($SkipBuild) { 'false' } else { 'true' }
    & $msbuild src\cascadia\CascadiaPackage\TmuxWorkspace.wapproj /m:6 /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$repo\" "/p:AppxPackageDir=$OutputDirectory\" "/p:BuildProjectReferences=$buildReferences" /p:UapAppxPackageBuildMode=SideloadOnly /p:AppxPackageSigningEnabled=false /p:AppxBundle=Never /nologo /verbosity:minimal
    if ($LASTEXITCODE) { throw 'MSIX packaging failed.' }
    $package = Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File -Filter "TmuxWorkspace_$($version)_x64.msix" |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $package) { throw 'The expected MSIX package was not generated.' }
    $destination = Join-Path $OutputDirectory "TmuxWorkspace-$version-x64.msix"
    Copy-Item -LiteralPath $package.FullName -Destination $destination -Force
    # Use the certificate store. No PFX or password is read, copied or passed.
    & $signtool sign /fd SHA256 /sha1 $CertificateThumbprint /s My $destination
    if ($LASTEXITCODE) { throw 'MSIX signing failed.' }
    & $signtool verify /pa /v $destination
    if ($LASTEXITCODE) { throw 'MSIX signature verification failed.' }
    Write-Host "Signed installer: $destination"
}
finally { Pop-Location }
