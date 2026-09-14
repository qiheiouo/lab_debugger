param(
    [Parameter(Mandatory = $true)]
    [string]$QtRoot,
    [string]$BuildDirectory = "build-competition-release",
    [string]$OutputDirectory = "dist/LabDebugger-AI-Competition",
    [string]$CMakeExecutable = "cmake"
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$qtRootPath = (Resolve-Path -LiteralPath $QtRoot).Path
$buildPath = [IO.Path]::GetFullPath((Join-Path $repositoryRoot $BuildDirectory))
$outputPath = [IO.Path]::GetFullPath((Join-Path $repositoryRoot $OutputDirectory))

if (Test-Path -LiteralPath $outputPath) {
    throw "输出目录已存在，请更换 OutputDirectory 或先人工确认后移走旧目录：$outputPath"
}

Push-Location $repositoryRoot
try {
    & $CMakeExecutable -S . -B $buildPath `
        -G "Visual Studio 17 2022" -A x64 `
        -DCMAKE_BUILD_TYPE=Release `
        -DLAB_DEBUGGER_BUILD_GUI=ON `
        -DLAB_DEBUGGER_BUILD_TESTS=ON `
        "-DCMAKE_CXX_FLAGS=/WX" `
        "-DCMAKE_PREFIX_PATH=$qtRootPath"
    if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }

    & $CMakeExecutable --build $buildPath --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "Release 构建失败" }

    & $CMakeExecutable --install $buildPath --config Release --prefix $outputPath
    if ($LASTEXITCODE -ne 0) { throw "便携目录生成失败" }

    $executable = Join-Path $outputPath "bin/LabDebugger.exe"
    if (-not (Test-Path -LiteralPath $executable)) {
        throw "便携目录中缺少 LabDebugger.exe"
    }
    $process = Start-Process -FilePath $executable `
        -ArgumentList "--smoke-test" `
        -WorkingDirectory (Split-Path $executable -Parent) `
        -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(10000)) {
        Stop-Process -Id $process.Id -Force
        throw "便携版启动检查超时"
    }
    if ($process.ExitCode -ne 0) {
        throw "便携版启动检查失败，退出码 $($process.ExitCode)"
    }

    $hash = Get-FileHash -LiteralPath $executable -Algorithm SHA256
    $hashLine = "$($hash.Hash)  bin/LabDebugger.exe"
    Set-Content -LiteralPath (Join-Path $outputPath "SHA256SUMS.txt") `
        -Value $hashLine -Encoding UTF8
    Write-Host "便携版生成成功：$outputPath"
    Write-Host $hashLine
}
finally {
    Pop-Location
}
