#Requires -Version 7
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $SshCommand,
    [string] $WorkDirectory,
    [string] $AiBaseUrl,
    [string] $JoinProbeDirectory,
    [switch] $TouchInput,
    [switch] $SkipBuild
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $WorkDirectory) { $WorkDirectory = Join-Path $repo 'artifacts\workflow' }
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$appDirectory = Join-Path $WorkDirectory 'app'
$report = Join-Path $WorkDirectory 'workflow-result.txt'
if (-not $JoinProbeDirectory) { $JoinProbeDirectory = Join-Path $WorkDirectory 'join-pixels' }
$JoinProbeDirectory = [IO.Path]::GetFullPath($JoinProbeDirectory)
New-Item -ItemType Directory -Path $JoinProbeDirectory -Force | Out-Null
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\amd64\MSBuild.exe'
Push-Location -LiteralPath $repo
try {
    if (-not $SkipBuild) {
        & $msbuild OpenConsole.slnx '/t:Terminal\Window\WindowsTerminal' /m:6 /p:Configuration=Release /p:Platform=x64 /p:TmuxWorkflowTests=true /nologo /verbosity:minimal
        if ($LASTEXITCODE) { throw 'Workflow build failed.' }
    }
    & $msbuild src\cascadia\WindowsTerminal\WindowsTerminal.vcxproj /t:_WTPrepareUnpackagedLayoutForRun /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$repo\" /p:BuildProjectReferences=false /p:TmuxWorkflowTests=true /nologo /verbosity:quiet
    if ($LASTEXITCODE) { throw 'Workflow layout failed.' }
    New-Item -ItemType Directory -Path $appDirectory -Force | Out-Null
    $layout = Join-Path $repo 'bin\x64\Release\WindowsTerminal'
    Get-ChildItem -LiteralPath $layout -File | Where-Object Extension -In '.exe', '.dll', '.pri', '.json', '.winmd' | Copy-Item -Destination $appDirectory -Force
    Get-ChildItem -LiteralPath $layout -Directory | Where-Object Name -NotIn '_xaml', 'settings' | Copy-Item -Destination $appDirectory -Recurse -Force
    New-Item -ItemType File -Path (Join-Path $appDirectory '.portable') -Force | Out-Null
    $isolatedSettings = Join-Path $appDirectory 'settings'
    New-Item -ItemType Directory -Path $isolatedSettings -Force | Out-Null
    if (-not (Test-Path -LiteralPath (Join-Path $isolatedSettings 'settings.json'))) {
        '{}' | Set-Content -LiteralPath (Join-Path $isolatedSettings 'settings.json') -Encoding utf8
    }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $appDirectory 'WindowsTerminal.exe'
    $start.WorkingDirectory = $appDirectory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $start.ArgumentList.Add('-w')
    $start.ArgumentList.Add('new')
    $start.Environment['TMUX_WORKFLOW_SERVER'] = $SshCommand
    $start.Environment['TMUX_WORKFLOW_REPORT'] = $report
    if ($AiBaseUrl) { $start.Environment['TMUX_WORKFLOW_AI_URL'] = $AiBaseUrl }
    if ($TouchInput) { $start.Environment['TMUX_WORKFLOW_TOUCH_INPUT'] = '1' }
    if ($JoinProbeDirectory) { $start.Environment['TMUX_WORKFLOW_JOIN_PROBE'] = [IO.Path]::GetFullPath($JoinProbeDirectory) }
    $process = [Diagnostics.Process]::Start($start)
    if (-not $process.WaitForExit(60000)) {
        if (-not $process.WaitForExit(60000)) {
            $process.Kill()
            if (Test-Path -LiteralPath $report) { Get-Content -LiteralPath $report }
            throw 'Workspace workflow timed out.'
        }
    }
    if (Test-Path -LiteralPath $report) { Get-Content -LiteralPath $report }
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $report) -or
        -not (Select-String -LiteralPath $report -SimpleMatch 'PASS: real workspace workflow completed' -Quiet) -or
        -not (Select-String -LiteralPath $report -SimpleMatch 'PIXELS: opaque join rendered without uncovered pixels' -Quiet) -or
        -not (Select-String -LiteralPath $report -SimpleMatch 'WHEEL: native VT scroll reached the pointed tmux pane' -Quiet) -or
        ($TouchInput -and (
            -not (Select-String -LiteralPath $report -SimpleMatch 'TOUCH: Windows touch gestures passed' -Quiet) -or
            -not (Select-String -LiteralPath $report -SimpleMatch 'COPY: long press and drag selected terminal output' -Quiet) -or
            -not (Select-String -LiteralPath $report -SimpleMatch 'PANE COPY: terminal long press copied the pointed inactive pane' -Quiet)))) {
        throw "Workspace workflow failed with exit code $($process.ExitCode)."
    }
}
finally { Pop-Location }
