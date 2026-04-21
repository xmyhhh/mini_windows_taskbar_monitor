param(
    [string]$Version = "",
    [ValidateSet("patch", "minor", "major")]
    [string]$Bump = "patch",
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [string]$ReleaseNotes = "",
    [string]$ReleaseNotesFile = "",
    [switch]$SkipPublish,
    [switch]$AllowDirty
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ProjectRoot = $PSScriptRoot
$BuildDir = Join-Path $ProjectRoot "build"
$DistDir = Join-Path $BuildDir "artifacts"
$ExecutableName = "minimal_taskbar_monitor.exe"
$DefaultNotesFile = Join-Path $ProjectRoot "release_notes.md"

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Invoke-CommandChecked {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$FailureMessage
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        if ([string]::IsNullOrWhiteSpace($FailureMessage)) {
            $FailureMessage = "Command failed: $FilePath $($Arguments -join ' ')"
        }
        throw $FailureMessage
    }
}

function Get-CommandOutput {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$FailureMessage
    )

    $output = & $FilePath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $details = ($output | Out-String).Trim()
        if ([string]::IsNullOrWhiteSpace($FailureMessage)) {
            $FailureMessage = "Command failed: $FilePath $($Arguments -join ' ')"
        }
        if (-not [string]::IsNullOrWhiteSpace($details)) {
            $FailureMessage = "$FailureMessage`n$details"
        }
        throw $FailureMessage
    }

    return (($output | ForEach-Object { $_.ToString().TrimEnd() }) -join "`n").Trim()
}

function Normalize-VersionTag {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ""
    }

    if ($Value.StartsWith("v")) {
        return $Value
    }

    return "v$Value"
}

function Get-LatestVersionTag {
    $tagList = Get-CommandOutput -FilePath "git" `
                                 -Arguments @("tag", "--list", "v*", "--sort=-v:refname") `
                                 -FailureMessage "Unable to read git tags."
    if ([string]::IsNullOrWhiteSpace($tagList)) {
        return $null
    }

    $tags = $tagList -split "`r?`n"
    foreach ($tag in $tags) {
        if ($tag -match '^v\d+\.\d+\.\d+$') {
            return $tag
        }
    }

    return $null
}

function Get-NextVersionTag {
    param(
        [string]$LatestTag,
        [string]$BumpType
    )

    if ([string]::IsNullOrWhiteSpace($LatestTag)) {
        return "v0.1.0"
    }

    if ($LatestTag -notmatch '^v(\d+)\.(\d+)\.(\d+)$') {
        throw "Latest tag '$LatestTag' is not in vX.Y.Z format."
    }

    [int]$major = $Matches[1]
    [int]$minor = $Matches[2]
    [int]$patch = $Matches[3]

    switch ($BumpType) {
        "major" {
            $major += 1
            $minor = 0
            $patch = 0
        }
        "minor" {
            $minor += 1
            $patch = 0
        }
        default {
            $patch += 1
        }
    }

    return "v$major.$minor.$patch"
}

function Get-GitHubRepoSlug {
    param([string]$RemoteUrl)

    if ($RemoteUrl -match 'github\.com[:/](?<owner>[^/]+)/(?<repo>[^/.]+?)(?:\.git)?$') {
        return "$($Matches.owner)/$($Matches.repo)"
    }

    throw "Unable to parse GitHub repository from origin remote: $RemoteUrl"
}

function Get-ReleaseNotesText {
    param(
        [string]$CurrentTag,
        [string]$PreviousTag,
        [string]$Notes,
        [string]$NotesFilePath
    )

    if (-not [string]::IsNullOrWhiteSpace($Notes)) {
        return $Notes.Trim()
    }

    if ([string]::IsNullOrWhiteSpace($NotesFilePath) -and (Test-Path -LiteralPath $DefaultNotesFile)) {
        $NotesFilePath = $DefaultNotesFile
    }

    if (-not [string]::IsNullOrWhiteSpace($NotesFilePath)) {
        if (-not (Test-Path -LiteralPath $NotesFilePath)) {
            throw "Release notes file not found: $NotesFilePath"
        }
        return (Get-Content -LiteralPath $NotesFilePath -Raw).Trim()
    }

    $logArguments = @("log", "--pretty=format:- %h %s")
    if (-not [string]::IsNullOrWhiteSpace($PreviousTag)) {
        $logArguments += "$PreviousTag..HEAD"
    } else {
        $logArguments += "-n"
        $logArguments += "20"
        $logArguments += "HEAD"
    }

    $logText = Get-CommandOutput -FilePath "git" `
                                 -Arguments $logArguments `
                                 -FailureMessage "Unable to generate release notes from git log."

    if ([string]::IsNullOrWhiteSpace($logText)) {
        return "Release $CurrentTag"
    }

    return $logText
}

function Ensure-CleanWorktree {
    if ($AllowDirty) {
        return
    }

    $status = Get-CommandOutput -FilePath "git" `
                                -Arguments @("status", "--short") `
                                -FailureMessage "Unable to read git status."
    if (-not [string]::IsNullOrWhiteSpace($status)) {
        throw "Working tree is not clean. Commit or stash changes first, or rerun with -AllowDirty."
    }
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host " Minimal Taskbar Monitor Release" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

Push-Location $ProjectRoot
try {
    $null = Get-CommandOutput -FilePath "git" `
                              -Arguments @("rev-parse", "--show-toplevel") `
                              -FailureMessage "Current folder is not a git repository."
    $originUrl = Get-CommandOutput -FilePath "git" `
                                   -Arguments @("remote", "get-url", "origin") `
                                   -FailureMessage "Git remote 'origin' is not configured."
    $repoSlug = Get-GitHubRepoSlug -RemoteUrl $originUrl

    Ensure-CleanWorktree

    Write-Step "Fetching tags from origin"
    Invoke-CommandChecked -FilePath "git" `
                          -Arguments @("fetch", "--tags", "origin") `
                          -FailureMessage "Unable to fetch tags from origin."

    $previousTag = Get-LatestVersionTag
    $tagName = Normalize-VersionTag -Value $Version
    if ([string]::IsNullOrWhiteSpace($tagName)) {
        $tagName = Get-NextVersionTag -LatestTag $previousTag -BumpType $Bump
    }

    if ($tagName -notmatch '^v\d+\.\d+\.\d+$') {
        throw "Version must be in vX.Y.Z format."
    }

    $tagExists = $false
    try {
        $null = Get-CommandOutput -FilePath "git" `
                                  -Arguments @("rev-parse", "--verify", "refs/tags/$tagName") `
                                  -FailureMessage ""
        $tagExists = $true
    } catch {
        $tagExists = $false
    }

    if ($tagExists) {
        throw "Tag '$tagName' already exists."
    }

    $releaseNotesText = Get-ReleaseNotesText -CurrentTag $tagName `
                                             -PreviousTag $previousTag `
                                             -Notes $ReleaseNotes `
                                             -NotesFilePath $ReleaseNotesFile

    if (-not $SkipPublish -and [string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
        throw "GITHUB_TOKEN is required to publish a GitHub Release. Set it, or rerun with -SkipPublish."
    }

    Write-Step "Configuring CMake ($Configuration)"
    Invoke-CommandChecked -FilePath "cmake" `
                          -Arguments @("-S", $ProjectRoot, "-B", $BuildDir, "-G", "Visual Studio 17 2022", "-A", "x64") `
                          -FailureMessage "CMake configure failed."

    Write-Step "Building $Configuration"
    Invoke-CommandChecked -FilePath "cmake" `
                          -Arguments @("--build", $BuildDir, "--config", $Configuration, "--parallel") `
                          -FailureMessage "CMake build failed."

    $exePath = Join-Path (Join-Path $BuildDir $Configuration) $ExecutableName
    if (-not (Test-Path -LiteralPath $exePath)) {
        $builtExe = Get-ChildItem -Path $BuildDir -Filter $ExecutableName -Recurse | Select-Object -First 1
        if ($null -eq $builtExe) {
            throw "Built executable not found."
        }
        $exePath = $builtExe.FullName
    }

    if (-not (Test-Path -LiteralPath $DistDir)) {
        New-Item -ItemType Directory -Path $DistDir | Out-Null
    }

    $packageDir = Join-Path $DistDir "minimal_taskbar_monitor-$tagName"
    $zipPath = Join-Path $DistDir "minimal_taskbar_monitor-$tagName-win64.zip"

    if (Test-Path -LiteralPath $packageDir) {
        Remove-Item -LiteralPath $packageDir -Recurse -Force
    }
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }

    Write-Step "Packaging release artifacts"
    New-Item -ItemType Directory -Path $packageDir | Out-Null
    Copy-Item -LiteralPath $exePath -Destination (Join-Path $packageDir $ExecutableName)

    $readmePath = Join-Path $ProjectRoot "README.md"
    if (Test-Path -LiteralPath $readmePath) {
        Copy-Item -LiteralPath $readmePath -Destination (Join-Path $packageDir "README.md")
    }

    Compress-Archive -Path (Join-Path $packageDir "*") `
                     -DestinationPath $zipPath `
                     -CompressionLevel Optimal

    Write-Step "Creating git tag $tagName"
    Invoke-CommandChecked -FilePath "git" `
                          -Arguments @("tag", "-a", $tagName, "-m", "Release $tagName") `
                          -FailureMessage "Unable to create git tag."
    Invoke-CommandChecked -FilePath "git" `
                          -Arguments @("push", "origin", $tagName) `
                          -FailureMessage "Unable to push git tag to origin."

    $releaseUrl = ""
    if (-not $SkipPublish) {
        Write-Step "Publishing GitHub Release"
        $commitSha = Get-CommandOutput -FilePath "git" `
                                       -Arguments @("rev-parse", "HEAD") `
                                       -FailureMessage "Unable to read current commit SHA."
        $headers = @{
            Authorization         = "Bearer $env:GITHUB_TOKEN"
            Accept                = "application/vnd.github+json"
            "X-GitHub-Api-Version" = "2022-11-28"
        }

        $releaseBody = @{
            tag_name         = $tagName
            target_commitish = $commitSha
            name             = $tagName
            body             = $releaseNotesText
            draft            = $false
            prerelease       = $false
        } | ConvertTo-Json -Depth 4

        $releaseResponse = Invoke-RestMethod -Method Post `
                                             -Uri "https://api.github.com/repos/$repoSlug/releases" `
                                             -Headers $headers `
                                             -ContentType "application/json; charset=utf-8" `
                                             -Body $releaseBody

        $uploadBaseUrl = $releaseResponse.upload_url -replace '\{\?name,label\}$', ""
        $assetName = [System.Uri]::EscapeDataString((Split-Path -Path $zipPath -Leaf))
        Invoke-RestMethod -Method Post `
                          -Uri "$uploadBaseUrl?name=$assetName" `
                          -Headers $headers `
                          -ContentType "application/zip" `
                          -InFile $zipPath

        $releaseUrl = $releaseResponse.html_url
    }

    Write-Host ""
    Write-Host "Release completed successfully." -ForegroundColor Green
    Write-Host "Version : $tagName" -ForegroundColor White
    Write-Host "Package : $zipPath" -ForegroundColor White
    if (-not [string]::IsNullOrWhiteSpace($releaseUrl)) {
        Write-Host "Release : $releaseUrl" -ForegroundColor White
    } elseif ($SkipPublish) {
        Write-Host "Publish : skipped (-SkipPublish)" -ForegroundColor Yellow
    }
} finally {
    Pop-Location
}
