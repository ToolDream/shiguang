# 下载并校验 Nim 官方 MinGW-w64 发行包，只把其中的 bin 目录加入后续步骤的 PATH。
# 两个摘要都由本仓库在本机实际下载核对过，与官方 mingw64.7z.sha256 一致。
# 参考固定 Leaf 提交 df53b244383bc79abd3d8a035d4ac29636602444 的 Windows 编译链准备方式。
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$minGwpArchiveSha = '04a535d1e0880364ca9609713744f1a9a36835c4ced834cf435c4dc4eca2159a'
$extractorSha = '256feca8e274e5da655e2a284fabafd9f554365eb164862089dacd4e8276d282'

$work = Join-Path $env:RUNNER_TEMP 'shiguang-mingw'
New-Item -ItemType Directory -Force -Path $work | Out-Null

$extractor = Join-Path $work '7zr.exe'
Invoke-WebRequest -UseBasicParsing 'https://github.com/ip7z/7zip/releases/download/26.04/7zr.exe' -OutFile $extractor
$extractorActual = (Get-FileHash -Algorithm SHA256 -LiteralPath $extractor).Hash.ToLowerInvariant()
if ($extractorActual -ne $extractorSha) {
    throw "7zr.exe 校验值不匹配：期望 $extractorSha，实际 $extractorActual"
}

$archive = Join-Path $work 'mingw64.7z'
Invoke-WebRequest -UseBasicParsing 'https://nim-lang.org/download/mingw64.7z' -OutFile $archive
$archiveActual = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
if ($archiveActual -ne $minGwpArchiveSha) {
    throw "mingw64.7z 校验值不匹配：期望 $minGwpArchiveSha，实际 $archiveActual"
}

& $extractor x $archive "-o$work/extracted" -y | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'MinGW 解压失败。' }

$gcc = Get-ChildItem (Join-Path $work 'extracted') -Recurse -Filter gcc.exe |
    Where-Object { $_.Directory.Name -eq 'bin' } | Select-Object -First 1
if (-not $gcc) { throw '未找到 MinGW gcc.exe。' }

$bin = $gcc.Directory.FullName
$target = (& (Join-Path $bin 'gcc.exe') -dumpmachine).Trim()
if (-not $target.StartsWith('x86_64')) { throw "需要 x86_64 MinGW，实际为 $target。" }

Add-Content -LiteralPath $env:GITHUB_PATH -Value $bin
"MinGW bin：$bin"
"gcc target：$target"
"mingw64.7z sha256：$archiveActual"
