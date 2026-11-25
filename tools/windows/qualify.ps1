<#
.SYNOPSIS
  Builds SAIGE on this Windows machine, runs the tests and the desktop smoke run, and writes a qualification report that says what the Vulkan adapter really was.
.DESCRIPTION
  Follows docs/windows-renderer-integration.md ("Build the presenting executable" and "Physical GPU verification"). The report records the exact commit, the adapters Vulkan reports each
  classified as software or physical, the result of each step, and whether the run can count as physical-GPU evidence: it can only if a physical adapter was present, the ctest run passed and
  `saige_desktop.exe --require-rendered --validate` exited 0. A run on a software adapter (for example the Lavapipe CI job) is reported as correctness evidence and never as GPU evidence.
  The script builds into build\windows-release, writes qualification\ under -OutDir and changes nothing else. Not yet run on Windows by its author; the first run on the owner's machine is the first test.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\windows\qualify.ps1 -OutDir C:\saige-qualification
#>
[CmdletBinding()]
param(
    [string]$Source = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$OutDir = (Join-Path $PWD 'qualification'),
    [int]$Frames = 240
)

$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Set-Location $Source

$report = [ordered]@{
    schema_version = 1
    started_utc    = (Get-Date).ToUniversalTime().ToString('o')
    commit         = (git rev-parse HEAD 2>$null)
    dirty          = [bool](git status --porcelain 2>$null)
    machine        = $env:COMPUTERNAME
    os             = (Get-CimInstance Win32_OperatingSystem).Caption
    adapters       = @()
    steps          = [ordered]@{}
}

function Invoke-Step([string]$name, [scriptblock]$body) {
    $log = Join-Path $OutDir ("$name.log")
    Write-Host "== $name"
    & $body *>&1 | Tee-Object -FilePath $log | Out-Host
    $code = $LASTEXITCODE
    $report.steps[$name] = [ordered]@{ exit_code = $code; passed = ($code -eq 0); log = $log }
}

# Adapters, classified.
$vk = Get-Command vulkaninfo -ErrorAction SilentlyContinue
if ($vk) {
    $text = (& $vk.Source --summary 2>&1) -join "`n"
    Set-Content -Path (Join-Path $OutDir 'vulkaninfo.txt') -Value $text
    $names = [regex]::Matches($text, 'deviceName\s*=\s*(.+)') | ForEach-Object { $_.Groups[1].Value.Trim() }
    $types = [regex]::Matches($text, 'deviceType\s*=\s*(\S+)') | ForEach-Object { $_.Groups[1].Value }
    for ($i = 0; $i -lt $names.Count; $i++) {
        $t = if ($i -lt $types.Count) { $types[$i] } else { 'unknown' }
        $software = ($t -match 'CPU') -or ($names[$i] -match 'llvmpipe|lavapipe|swiftshader|WARP|Basic Render')
        $report.adapters += [ordered]@{ name = $names[$i]; type = $t; classification = $(if ($software) { 'software' } else { 'physical' }) }
    }
} else {
    $report.adapters = @()
    Write-Warning 'vulkaninfo not found (install the Vulkan SDK): adapters cannot be classified, so this run cannot count as GPU evidence.'
}

Invoke-Step 'configure' { cmake --preset windows-release -DSAIGE_ENABLE_VULKAN=ON -DSAIGE_VULKAN_FETCH_HEADERS=ON -DVULKAN_HEADERS_ENABLE_MODULE=OFF -DSAIGE_TEST_WIN32_VULKAN=ON }
Invoke-Step 'build'     { cmake --build --preset windows-release }
Invoke-Step 'ctest'     { ctest --preset windows-release --output-on-failure }
$exe = Join-Path $Source 'build\windows-release\Release\saige_desktop.exe'
if (Test-Path $exe) {
    Invoke-Step 'desktop_smoke' { & $exe --require-rendered --validate --frames $Frames }
} else {
    $report.steps['desktop_smoke'] = [ordered]@{ exit_code = $null; passed = $false; log = $null; note = "missing $exe" }
}

$physicalPresent = [bool]($report.adapters | Where-Object { $_.classification -eq 'physical' })
$allPassed = -not [bool]($report.steps.Values | Where-Object { -not $_.passed })
$report.finished_utc = (Get-Date).ToUniversalTime().ToString('o')
$report.physical_adapter_present = $physicalPresent
$report.qualifies_as_physical_gpu_evidence = ($physicalPresent -and $allPassed)
$report.note = 'Offscreen and presented frames only prove what they show; this report says nothing about frame rate. A run needs the displayed window observed by a person for the interactive checks (resize, minimize, input) listed in docs/windows-renderer-integration.md.'

$report | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $OutDir 'qualification.json')
Write-Host ('Report: ' + (Join-Path $OutDir 'qualification.json'))
Write-Host ('Physical adapter present: {0}; all steps passed: {1}; counts as physical GPU evidence: {2}' -f $physicalPresent, $allPassed, $report.qualifies_as_physical_gpu_evidence)
if (-not $allPassed) { exit 1 }
