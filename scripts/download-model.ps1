<#
.SYNOPSIS
    Downloads a bundled whisper.cpp GGML model into .\models.

.DESCRIPTION
    Recommended models (published at https://huggingface.co/ggerganov/whisper.cpp):
        small-q5_1            ~180 MB  (good size/quality tradeoff, multilingual)
        medium-q5_0           ~540 MB  (better quality)
        large-v3-q5_0         ~1.1 GiB  (best quality)

.EXAMPLE
    .\scripts\download-model.ps1
    .\scripts\download-model.ps1 large-v3-turbo-q5_0
#>
param(
    [string]$Model = "small-q5_1"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ModelsDir = Join-Path $ScriptDir "..\models"
$BaseUrl   = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main"
$File      = "ggml-$Model.bin"
$Url       = "$BaseUrl/$File"

New-Item -ItemType Directory -Force -Path $ModelsDir | Out-Null
$Dest = Join-Path $ModelsDir $File

if (Test-Path $Dest) {
    Write-Host "Model already present: $Dest"
    exit 0
}

Write-Host "Downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile "$Dest.part"
Move-Item -Force "$Dest.part" $Dest
Write-Host "Saved model to $Dest"
Write-Host ""
Write-Host "Point voiceTyper at it via Settings, or set modelPath in the config."
