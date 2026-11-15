#Requires -Version 7
[CmdletBinding()]
param(
    [string] $WorkDirectory,
    [switch] $WithSshFixture,
    [switch] $WithAiFixture,
    [switch] $WithXaml
)
$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $WorkDirectory) { $WorkDirectory = Join-Path $repo 'artifacts\tests' }
$WorkDirectory = [System.IO.Path]::GetFullPath($WorkDirectory)
New-Item -ItemType Directory -Path $WorkDirectory -Force | Out-Null
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio C++ tools were not found.' }
Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64' | Out-Null
Push-Location -LiteralPath $repo
try {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /WX src\cascadia\LocalTests_TmuxWorkspace\tests.cpp "/Fo:$WorkDirectory\tmux-tests.obj" "/Fe:$WorkDirectory\tmux-tests.exe" /link shell32.lib
    if ($LASTEXITCODE) { throw 'Core test compilation failed.' }
    & (Join-Path $WorkDirectory 'tmux-tests.exe')
    if ($LASTEXITCODE) { throw 'Core tests failed.' }
    if ($WithXaml) {
        Copy-Item -LiteralPath (Join-Path $repo 'bin\x64\Release\Microsoft.Terminal.Settings.Model\Microsoft.Terminal.Settings.Model.dll') -Destination $WorkDirectory -Force
        & cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /WX "/I$repo\src\cascadia\TerminalApp\Generated Files" src\cascadia\LocalTests_TmuxWorkspace\ui_tests.cpp "/Fo:$WorkDirectory\tmux-ui-tests.obj" "/Fe:$WorkDirectory\tmux-ui-tests.exe" /link windowsapp.lib /MANIFEST:EMBED /MANIFESTINPUT:src\cascadia\LocalTests_TmuxWorkspace\ui_tests.manifest
        if ($LASTEXITCODE) { throw 'XAML regression test compilation failed.' }
        & (Join-Path $WorkDirectory 'tmux-ui-tests.exe')
        if ($LASTEXITCODE) { throw 'XAML regression tests failed.' }
    }
    if ($WithSshFixture) {
        # Link the actual production SSH object, its PCH, and the same CLI11.
        & cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /MT src\cascadia\LocalTests_TmuxWorkspace\ssh_tests.cpp obj\x64\Release\TerminalAppLib\TmuxWorkspace.obj obj\x64\Release\TerminalAppLib\pch.obj obj\x64\vcpkg\x64-windows-static\lib\CLI11.lib "/Fo:$WorkDirectory\tmux-ssh-tests.obj" "/Fe:$WorkDirectory\tmux-ssh-tests.exe" /link /LTCG /NODEFAULTLIB:libucrt.lib /DEFAULTLIB:ucrt.lib WindowsApp.lib
        if ($LASTEXITCODE) { throw 'SSH test compilation failed. Build Release x64 first.' }
        & python src\cascadia\LocalTests_TmuxWorkspace\ssh_fixture.py (Join-Path $WorkDirectory 'tmux-ssh-tests.exe') --work-directory $WorkDirectory
        if ($LASTEXITCODE) { throw 'SSH tests failed. The optional fixture requires Python and Paramiko.' }
    }
    if ($WithAiFixture) {
        & cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /MT src\cascadia\LocalTests_TmuxWorkspace\ai_tests.cpp obj\x64\Release\TerminalAppLib\TmuxAi.obj obj\x64\Release\TerminalAppLib\pch.obj obj\x64\vcpkg\x64-windows-static\lib\jsoncpp.lib obj\x64\vcpkg\x64-windows-static\lib\CLI11.lib "/Fo:$WorkDirectory\tmux-ai-tests.obj" "/Fe:$WorkDirectory\tmux-ai-tests.exe" /link /LTCG /NODEFAULTLIB:libucrt.lib /DEFAULTLIB:ucrt.lib WindowsApp.lib winhttp.lib crypt32.lib
        if ($LASTEXITCODE) { throw 'AI test compilation failed. Build Release x64 first.' }
        & python src\cascadia\LocalTests_TmuxWorkspace\ai_fixture.py -- (Join-Path $WorkDirectory 'tmux-ai-tests.exe') '{base_url}' (Join-Path $WorkDirectory 'isolated-ai.json')
        if ($LASTEXITCODE) { throw 'AI API fixture tests failed.' }
    }
}
finally {
    Pop-Location
}
