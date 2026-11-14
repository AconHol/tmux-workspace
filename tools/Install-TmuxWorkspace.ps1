[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $PackagePath,
    [string] $MigrateFromDirectory,
    [switch] $Launch,
    [switch] $Restart
)
$ErrorActionPreference = 'Stop'
$PackagePath = (Resolve-Path -LiteralPath $PackagePath).Path
$signature = Get-AuthenticodeSignature -LiteralPath $PackagePath
if ($signature.Status -ne 'Valid') {
    throw 'The installer signature is not trusted or valid on this computer. Verify the publisher before installing.'
}
if ($Restart) { Add-AppxPackage -Path $PackagePath -ForceApplicationShutdown -ErrorAction Stop }
else { Add-AppxPackage -Path $PackagePath -ErrorAction Stop }
$installedPackage = Get-AppxPackage -Name TmuxWorkspace | Sort-Object Version -Descending | Select-Object -First 1
if (-not $installedPackage) { throw 'The package did not register for the current Windows user.' }
$settingsDirectory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ("Packages\" + $installedPackage.PackageFamilyName + '\LocalState')
$migratedFiles = 0
if ($MigrateFromDirectory -and (Test-Path -LiteralPath $MigrateFromDirectory)) {
    New-Item -ItemType Directory -Path $settingsDirectory -Force | Out-Null
    foreach ($name in 'settings.json', 'tmux-servers.json') {
        $source = Join-Path $MigrateFromDirectory $name
        $destination = Join-Path $settingsDirectory $name
        if ((Test-Path -LiteralPath $source) -and -not (Test-Path -LiteralPath $destination)) {
            Copy-Item -LiteralPath $source -Destination $destination
            $migratedFiles++
        }
    }
}
if ($Launch) {
    $launcherShell = New-Object -ComObject Shell.Application
    $launcherShell.ShellExecute("shell:AppsFolder\$($installedPackage.PackageFamilyName)!App", '', '', 'open', 1)
}
[PSCustomObject]@{
    Name = $installedPackage.Name
    Version = [string]$installedPackage.Version
    Publisher = $installedPackage.Publisher
    Status = [string]$installedPackage.Status
    PackageFamilyName = $installedPackage.PackageFamilyName
    InstallLocation = $installedPackage.InstallLocation
    MigratedSettingsFiles = $migratedFiles
    AppUserModelId = $installedPackage.PackageFamilyName + '!App'
}
