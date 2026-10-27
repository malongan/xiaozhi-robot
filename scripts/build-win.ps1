# 一键构建（Windows）。
#
# 自动做三件每次都容易忘的事：
#   1. 把 ESP-IDF 自带的 python 环境前置到 PATH（系统里的 3.13 会让 export 找不到依赖）
#   2. 加载 export.ps1
#   3. 调用 feature_build.py（内部已处理输出落文件、默认并行度、卡死重试）
#
# 用法：
#   .\scripts\build-win.ps1                      # 默认 media 组合
#   .\scripts\build-win.ps1 -Profile daily
#   .\scripts\build-win.ps1 -Profile media -Jobs 4
#
# 首次使用可能需要放开执行策略：
#   Set-ExecutionPolicy -Scope CurrentUser RemoteSigned

param(
    [string]$Profile = "media",
    [int]$Jobs = 0,
    [string]$IdfPath = ""
)

$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent $PSScriptRoot

# --- 定位 ESP-IDF ---
if (-not $IdfPath) {
    if ($env:IDF_PATH -and (Test-Path (Join-Path $env:IDF_PATH "export.ps1"))) {
        $IdfPath = $env:IDF_PATH
    }
    elseif (Test-Path "D:\esp-idf\export.ps1") {
        $IdfPath = "D:\esp-idf"
    }
    else {
        throw "找不到 ESP-IDF，请用 -IdfPath 指定安装目录"
    }
}

# --- 前置 IDF 自带的 python 环境 ---
$pyRoot = Join-Path $env:USERPROFILE ".espressif\python_env"
$pyEnv = $null
if (Test-Path $pyRoot) {
    $pyEnv = Get-ChildItem $pyRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "idf*_py*_env" } |
        Sort-Object Name -Descending |
        Select-Object -First 1
}
if ($pyEnv) {
    $env:PATH = (Join-Path $pyEnv.FullName "Scripts") + ";" + $env:PATH
    Write-Output ("python env: " + $pyEnv.Name)
}

# --- 加载 IDF 环境 ---
# export.ps1 会往 stderr 打激活信息，别让 ErrorActionPreference 把它当异常中断
Write-Output ("idf: " + $IdfPath)
. (Join-Path $IdfPath "export.ps1")

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "ESP-IDF 环境加载失败，PATH 里没有 idf.py"
}

# --- 构建 ---
$cmdArgs = @("scripts\feature_build.py", "--profile", $Profile)
if ($Jobs -gt 0) { $cmdArgs += @("--jobs", "$Jobs") }
$buildRoot = $env:XIAOZHI_BUILD_ROOT
if (-not $buildRoot) { $buildRoot = [Environment]::GetEnvironmentVariable("XIAOZHI_BUILD_ROOT", "User") }
if ($buildRoot) { Write-Output ("build root: " + $buildRoot) }

Push-Location $repo
try {
    & python @cmdArgs
    $code = $LASTEXITCODE
}
finally {
    Pop-Location
}
exit $code
