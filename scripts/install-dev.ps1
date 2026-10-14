# SPDX-License-Identifier: MIT
#
# Development install of OpenA2DP on a test machine (Windows PowerShell 5.1+,
# run as administrator). Uses the CI artifacts downloaded with
#
#   gh run download <run-id> -D dist
#
# which gives dist\drivers-x64\{oa2dp,oa2dpaudio}\ (sys, inf, unsigned cat)
# and dist\user-mode-x64\ (oa2dpsvc.exe, oa2dpctl.exe, oa2dpgui.exe).
#
#   .\scripts\install-dev.ps1              sign, add the driver packages,
#                                          install and start the service
#   .\scripts\install-dev.ps1 -Uninstall   undo all of that (Windows' own
#                                          driver goes back on every headset)
#
# Prerequisite (once): test signing must be on, which needs Secure Boot off
# in the firmware, then:  bcdedit /set testsigning on  and a reboot.
# The test certificate is created in the machine store and trusted there;
# -Uninstall removes it again.

[CmdletBinding()]
param(
    [string]$Dist = (Join-Path $PSScriptRoot '..\dist'),
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'OpenA2DP'),
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$CertSubject = 'CN=OpenA2DP Test Signing'
$ServiceName = 'oa2dpsvc'

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an elevated PowerShell (Run as administrator).'
    }
}

function Get-SignTool {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $st = Get-ChildItem (Join-Path $kits '*\x64\signtool.exe') -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -Last 1
    if (-not $st) { throw "signtool.exe not found under $kits (install the Windows SDK signing tools)." }
    return $st.FullName
}

function Get-OurDriverPackages {
    # Published names (oemNN.inf) of our packages in the driver store. DISM
    # objects rather than pnputil text, which is localized.
    Get-WindowsDriver -Online |
        Where-Object { (Split-Path $_.OriginalFileName -Leaf) -in 'oa2dp.inf', 'oa2dpaudio.inf' } |
        ForEach-Object { $_.Driver }
}

function Test-TestSigning {
    # Options of the current boot (bcdedit's text output is localized).
    $opts = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name SystemStartOptions).SystemStartOptions
    return $opts -match 'TESTSIGNING'
}

Assert-Admin

if ($Uninstall) {
    $ctl = Join-Path $InstallDir 'oa2dpctl.exe'
    if (Test-Path $ctl) {
        Write-Host '== Putting Windows'' driver back on every headset'
        & $ctl restore all
    }
    if (Get-Service $ServiceName -ErrorAction SilentlyContinue) {
        Write-Host "== Removing service $ServiceName"
        Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue
        & sc.exe delete $ServiceName | Out-Null
        Start-Sleep -Seconds 1
    }
    foreach ($pkg in Get-OurDriverPackages) {
        Write-Host "== Removing driver package $pkg"
        & pnputil.exe /delete-driver $pkg /uninstall /force
    }
    if (Test-Path $InstallDir) { Remove-Item $InstallDir -Recurse -Force }
    foreach ($store in 'My', 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $CertSubject } |
            Remove-Item
    }
    Write-Host 'Done. You can turn test signing off with: bcdedit /set testsigning off (then reboot).'
    return
}

$drivers = Join-Path $Dist 'drivers-x64'
$usermode = Join-Path $Dist 'user-mode-x64'
foreach ($f in 'oa2dp\oa2dp.sys', 'oa2dp\oa2dp.inf', 'oa2dp\oa2dp.cat',
               'oa2dpaudio\oa2dpaudio.sys', 'oa2dpaudio\oa2dpaudio.inf', 'oa2dpaudio\oa2dpaudio.cat') {
    if (-not (Test-Path (Join-Path $drivers $f))) { throw "Missing $drivers\$f (download the CI artifacts first)." }
}
foreach ($f in 'oa2dpsvc.exe', 'oa2dpctl.exe', 'oa2dpgui.exe') {
    if (-not (Test-Path (Join-Path $usermode $f))) { throw "Missing $usermode\$f (download the CI artifacts first)." }
}

if (-not (Test-TestSigning)) {
    throw ('Test signing is off. Turn Secure Boot off in the firmware, run ' +
           '"bcdedit /set testsigning on" from an elevated prompt, reboot, then run this script again.')
}

Write-Host '== Test certificate'
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $CertSubject } |
    Sort-Object NotAfter | Select-Object -Last 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
        -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(5)
}
$cer = Join-Path $env:TEMP 'oa2dp-test.cer'
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
foreach ($store in 'Root', 'TrustedPublisher') {
    if (-not (Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })) {
        Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
    }
}
Remove-Item $cer

Write-Host '== Signing drivers and catalogs'
$signtool = Get-SignTool
foreach ($d in 'oa2dp', 'oa2dpaudio') {
    $dir = Join-Path $drivers $d
    foreach ($f in "$d.sys", "$d.cat") {
        & $signtool sign /sm /s My /sha1 $cert.Thumbprint /fd sha256 (Join-Path $dir $f)
        if ($LASTEXITCODE -ne 0) { throw "signtool failed on $d\$f" }
    }
}

Write-Host '== Adding the driver packages to the driver store'
# oa2dp.inf is only added: Windows keeps its own driver on every headset
# until you pick ours for one with "oa2dpctl use <dev>" (or the GUI).
& pnputil.exe /add-driver (Join-Path $drivers 'oa2dp\oa2dp.inf')
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw 'pnputil failed for oa2dp.inf' }
# oa2dpaudio.inf only matches OA2DP\Render, which oa2dp.sys creates.
& pnputil.exe /add-driver (Join-Path $drivers 'oa2dpaudio\oa2dpaudio.inf') /install
# 259 = no matching device yet (normal before "oa2dpctl use"), 3010 = reboot.
if ($LASTEXITCODE -notin 0, 259, 3010) { throw 'pnputil failed for oa2dpaudio.inf' }

Write-Host "== Installing the service and tools to $InstallDir"
if (Get-Service $ServiceName -ErrorAction SilentlyContinue) {
    Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force $InstallDir | Out-Null
Copy-Item (Join-Path $usermode '*.exe') $InstallDir -Force
$svcExe = Join-Path $InstallDir 'oa2dpsvc.exe'
if (-not (Get-Service $ServiceName -ErrorAction SilentlyContinue)) {
    New-Service -Name $ServiceName -BinaryPathName "`"$svcExe`"" -StartupType Automatic `
        -DisplayName 'OpenA2DP Service' `
        -Description 'Streams audio to headsets bound to the OpenA2DP driver.' | Out-Null
}
Start-Service $ServiceName

Write-Host ''
Write-Host 'Installed. Next:'
Write-Host "  & '$InstallDir\oa2dpctl.exe' list          # find your headset"
Write-Host "  & '$InstallDir\oa2dpctl.exe' use <n|addr>  # switch it to OpenA2DP"
Write-Host "  or open $InstallDir\oa2dpgui.exe"
Write-Host "Service log: $env:ProgramData\OpenA2DP\oa2dpsvc.log"
Write-Host 'To undo everything: .\scripts\install-dev.ps1 -Uninstall'
