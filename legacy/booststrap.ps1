# due to create symboliclink, this script needs to be run in administrator
$osArch = (Get-WmiObject Win32_OperatingSystem).OSArchitecture
# create symbolic link to settings.json
if ($osArch -like "*x64*") {
    Write-Host "Windows architecture is x64 (64-bit)."
    $settingsPath = Join-Path $PSScriptRoot ".vscode/win_x86_64_setting.json"
    if (-Not (Test-Path -Path $settingsPath)) {
        Write-Host "win_x86_64_setting.json does not exist. Please create it." 
        exit 1
    }
    New-Item -ItemType SymbolicLink -Path "$PSScriptRoot\.vscode\settings.json" -Target $settingsPath -Force | Out-Null
    Write-Host "Symbolic link created for win_x86_64_setting.json." 

} elseif ($osArch -like "*ARM*") {
    Write-Host "Windows architecture is ARM."
    $settingsPath = Join-Path $PSScriptRoot ".vscode/win_on_arm_settings.json"
    if (-Not (Test-Path -Path $settingsPath)) {
        Write-Host "win_on_arm_settings.json does not exist. Please create it." 
        exit 1
    }
    New-Item -ItemType SymbolicLink -Path "$PSScriptRoot\.vscode\settings.json" -Target $settingsPath -Force | Out-Null
    Write-Host "Symbolic link created for win_on_arm_settings.json." 
} elseif ($osArch -like "*x86*"){
    Write-Host "Windows architecture is x86 (32-bit)."
    $settingsPath = Join-Path $PSScriptRoot ".vscode/win_x86_setting.json"
    if (-Not (Test-Path -Path $settingsPath)) {
        Write-Host "win_x86_setting.json does not exist. Please create it." 
        exit 1
    }
    New-Item -ItemType SymbolicLink -Path "$PSScriptRoot\.vscode\settings.json" -Target $settingsPath -Force | Out-Null
    Write-Host "Symbolic link created for win_x86_setting.json." 

} else {
   Write-Debug "Unknown Windows architecture: $osArch"
   exit 1
}

# create symbolic link to compile_commands.json
$buildPath = Join-Path $PSScriptRoot "build"
if (-Not (Test-Path -Path $buildPath)) {
    New-Item -ItemType Directory -Path $buildPath | Out-Null
}
New-Item -ItemType SymbolicLink -Path "$PSScriptRoot\compile_commands.json" -Target "$buildPath\compile_commands.json" -Force | Out-Null
Write-Host "Symbolic link created for compile_commands.json."