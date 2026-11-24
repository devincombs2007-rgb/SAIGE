<#
.SYNOPSIS
  Checks (and with -Install, installs) what a Windows x64 machine needs to build and qualify SAIGE.
.DESCRIPTION
  Default is read-only: it prints what is present and what is missing. With -Install it asks winget for the missing pieces (winget is Microsoft's package manager; it needs no secrets and
  downloads only from its own catalog). It never changes system settings, never installs a GPU driver (install that from the GPU vendor) and never runs elevated on its own: run an elevated
  PowerShell yourself if winget asks for it.
  Not yet run on a Windows machine by its author (the authoring environment is Linux): the first run on the owner's machine is the first test. Report anything it gets wrong in issue #43.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\windows\bootstrap.ps1
  powershell -ExecutionPolicy Bypass -File tools\windows\bootstrap.ps1 -Install
#>
[CmdletBinding()]
param([switch]$Install)

$ErrorActionPreference = 'Stop'

function Get-FirstVersion([string]$text) {
    if ($text -match '(\d+(\.\d+)+)') { return [version]$Matches[1] }
    return $null
}

function Test-Tool([string]$name, [string[]]$versionArgs, [version]$minimum) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if (-not $cmd) { return [pscustomobject]@{ Name = $name; Found = $false; Version = $null; Ok = $false; Minimum = $minimum } }
    $v = $null
    try { $v = Get-FirstVersion ((& $cmd.Source @versionArgs 2>&1) -join "`n") } catch { }
    $ok = $true
    if ($minimum -and ($null -eq $v -or $v -lt $minimum)) { $ok = $false }
    return [pscustomobject]@{ Name = $name; Found = $true; Version = $v; Ok = $ok; Minimum = $minimum }
}

# Visual Studio 2022 with the C++ x64 toolset, found the supported way (vswhere ships with the installer).
function Test-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $false }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    return [bool]$path
}

function Test-VulkanLoader {
    return (Test-Path (Join-Path $env:SystemRoot 'System32\vulkan-1.dll'))
}

$checks = @(
    (Test-Tool 'git' @('--version') ([version]'2.30')),
    (Test-Tool 'cmake' @('--version') ([version]'3.21')),
    (Test-Tool 'ninja' @('--version') $null),
    (Test-Tool 'python' @('--version') ([version]'3.10')),
    (Test-Tool 'vulkaninfo' @('--summary') $null)
)
$vs = Test-VisualStudio
$loader = Test-VulkanLoader

Write-Host 'SAIGE Windows prerequisites'
foreach ($c in $checks) {
    $state = if (-not $c.Found) { 'missing' } elseif ($c.Ok) { 'ok' } else { 'too old' }
    $ver = if ($c.Version) { $c.Version } else { '-' }
    Write-Host ('  {0,-12} {1,-8} {2}' -f $c.Name, $state, $ver)
}
Write-Host ('  {0,-12} {1}' -f 'Visual Studio 2022 C++ x64 tools', $(if ($vs) { 'ok' } else { 'missing' }))
Write-Host ('  {0,-12} {1}' -f 'Vulkan loader (vulkan-1.dll)', $(if ($loader) { 'ok' } else { 'missing: install the GPU vendor driver and the Vulkan SDK' }))
Write-Host 'The GPU driver itself is not installed by this script: use the vendor (NVIDIA, AMD or Intel) installer.'

if (-not $Install) {
    Write-Host 'Dry run only. Re-run with -Install to fetch the missing tools with winget.'
    return
}

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { throw 'winget is not available; install the "App Installer" from the Microsoft Store, then re-run.' }

$wanted = @{}
foreach ($c in $checks) { if (-not $c.Ok -and $c.Name -ne 'vulkaninfo') { $wanted[$c.Name] = $true } }
$ids = @()
if ($wanted['git']) { $ids += 'Git.Git' }
if ($wanted['cmake']) { $ids += 'Kitware.CMake' }
if ($wanted['ninja']) { $ids += 'Ninja-build.Ninja' }
if ($wanted['python']) { $ids += 'Python.Python.3.12' }
if (-not $loader -or ($checks | Where-Object { $_.Name -eq 'vulkaninfo' -and -not $_.Found })) { $ids += 'KhronosGroup.VulkanSDK' }
foreach ($id in $ids) {
    Write-Host "winget install $id"
    winget install --id $id --exact --accept-package-agreements --accept-source-agreements
}
if (-not $vs) {
    Write-Host 'winget install Microsoft.VisualStudio.2022.BuildTools (C++ workload)'
    winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --accept-package-agreements --accept-source-agreements --override '--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
}
Write-Host 'Done. Open a new terminal so PATH changes apply, then run tools\windows\qualify.ps1.'
