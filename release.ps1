param(
    [string]$Version = "",
    [string]$ReleaseNotes = ""
)

$ErrorActionPreference = "Stop"

$ProjectRoot = $PSScriptRoot
$BuildDir = Join-Path $ProjectRoot "build"
$SrcDir = Join-Path $ProjectRoot "src"

if ([string]::IsNullOrEmpty($Version)) {
    $Version = Get-Date -Format "yyyy.MMdd.HHmm"
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Release Script" -ForegroundColor Cyan
Write-Host "  Version: $Version" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

Write-Host "`n[1/4] Cleaning build directory..." -ForegroundColor Yellow
if (Test-Path $BuildDir) {
    Remove-Item -Recurse -Force $BuildDir
}
New-Item -ItemType Directory -Path $BuildDir | Out-Null

Write-Host "`n[2/4] Configuring CMake (Release)..." -ForegroundColor Yellow
Push-Location $BuildDir
cmake .. -DCMAKE_BUILD_TYPE=Release -G "Visual Studio 17 2022"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

Write-Host "`n[3/4] Building Release..." -ForegroundColor Yellow
cmake --build . --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
Pop-Location

$ExePath = Join-Path $BuildDir "Releaseminimal_taskbar_monitor.exe"
if (-not (Test-Path $ExePath)) {
    $ExePath = Join-Path $BuildDir "minimal_taskbar_monitor.exe"
}
if (-not (Test-Path $ExePath)) {
    $ExePath = (Get-ChildItem $BuildDir -Filter "*.exe" -Recurse | Select-Object -First 1).FullName
}

if (-not (Test-Path $ExePath)) {
    throw "Executable not found after build"
}

Write-Host "`n[4/4] Creating Git release..." -ForegroundColor Yellow

git checkout main 2>$null
git pull origin main

$TagName = "v$Version"

git tag -a $TagName -m "Release $Version"
git push origin $TagName

$AssetPath = $ExePath
$ReleaseNotesPath = Join-Path $ProjectRoot "release_notes.txt"
if ([string]::IsNullOrEmpty($ReleaseNotes)) {
    if (Test-Path $ReleaseNotesPath) {
        $ReleaseNotes = Get-Content $ReleaseNotesPath -Raw
    } else {
        $ReleaseNotes = "Release $Version"
    }
}

$Token = $env:GITHUB_TOKEN
if ([string]::IsNullOrEmpty($Token)) {
    $RepoUrl = git remote get-url origin
    Write-Host "`n========================================" -ForegroundColor Yellow
    Write-Host "  Git tag '$TagName' pushed successfully!" -ForegroundColor Green
    Write-Host "  Please create GitHub release manually:" -ForegroundColor Yellow
    Write-Host "  1. Go to: $RepoUrl/releases/new?tag=$TagName" -ForegroundColor White
    Write-Host "  2. Upload: $AssetPath" -ForegroundColor White
    Write-Host "========================================" -ForegroundColor Yellow
} else {
    $Repo = (git remote get-url origin) -replace '.*[:/]([^/]+/[^/]+?)(?:\.git)?$', '$1'
    $ApiUrl = "https://api.github.com/repos/$Repo/releases"

    $Body = @{
        tag_name = $TagName
        name = "Release $Version"
        body = $ReleaseNotes
        draft = $false
        prerelease = $false
    } | ConvertTo-Json

    Write-Host "Creating GitHub release..." -ForegroundColor Yellow
    $Response = Invoke-RestMethod -Uri $ApiUrl -Method Post -Headers @{
        Authorization = "token $Token"
        Accept = "application/vnd.github.v3+json"
    } -Body $Body -ContentType "application/json"

    if ($Response.id) {
        $UploadUrl = $Response.upload_url -replace '\{\?name,label\}', "?name=$(Split-Path $AssetPath -Leaf)"
        Invoke-RestMethod -Uri $UploadUrl -Method Post -Headers @{
            Authorization = "token $Token"
            Content-Type = "application/octet-stream"
        } -Body ([System.IO.File]::ReadAllBytes($AssetPath))
        Write-Host "Asset uploaded successfully!" -ForegroundColor Green
    }
}

Write-Host "`n========================================" -ForegroundColor Green
Write-Host "  Release completed successfully!" -ForegroundColor Green
Write-Host "  Version: $Version" -ForegroundColor White
Write-Host "  Executable: $ExePath" -ForegroundColor White
Write-Host "========================================" -ForegroundColor Green
