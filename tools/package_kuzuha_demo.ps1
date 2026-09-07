param(
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$ShaderDirectory,
    [Parameter(Mandatory=$true)][string]$Model,
    [Parameter(Mandatory=$true)][string]$TextureDirectory,
    [Parameter(Mandatory=$true)][string]$Destination
)
$ErrorActionPreference = 'Stop'
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = $Utf8NoBom
$OutputEncoding = $Utf8NoBom
foreach ($file in @($Binary, $Model)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing input: $file" }
}
$shaders = @('model.vert.spv', 'reconstruction.frag.spv', 'bone.vert.spv', 'bone.frag.spv',
    'pn_ray.vert.spv', 'pn_ray.frag.spv', 'text_overlay.vert.spv', 'text_overlay.frag.spv')
foreach ($name in $shaders) {
    if (-not (Test-Path -LiteralPath (Join-Path $ShaderDirectory $name))) { throw "Missing shader: $name" }
}
$textures = @(Get-ChildItem -LiteralPath $TextureDirectory -File | Where-Object { $_.Name -like '*_bsc.png' })
if ($textures.Count -eq 0) { throw 'No diffuse *_bsc.png textures' }
$target = [System.IO.Path]::GetFullPath($Destination)
foreach ($part in @('', 'model', 'model/texture', 'shaders', 'results')) {
    New-Item -ItemType Directory -Path (Join-Path $target $part) -Force | Out-Null
}
Copy-Item -LiteralPath $Binary -Destination (Join-Path $target 'pictor_kuzuha_demo.exe')
Copy-Item -LiteralPath $Model -Destination (Join-Path $target 'model/model.fbx')
foreach ($name in $shaders) {
    Copy-Item -LiteralPath (Join-Path $ShaderDirectory $name) -Destination (Join-Path $target 'shaders')
}
foreach ($texture in $textures) { Copy-Item -LiteralPath $texture.FullName -Destination (Join-Path $target 'model/texture') }
$repoRoot = Split-Path $PSScriptRoot -Parent
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $target 'LICENSE.txt')
$glfwLicense = Join-Path $repoRoot 'build/_deps/glfw-src/LICENSE.md'
if (Test-Path -LiteralPath $glfwLicense) { Copy-Item -LiteralPath $glfwLicense -Destination (Join-Path $target 'GLFW-LICENSE.md') }
Copy-Item -LiteralPath (Join-Path $repoRoot 'third_party/stb/stb_image.h') -Destination (Join-Path $target 'STB-NOTICE.h')
Copy-Item -LiteralPath (Join-Path $repoRoot 'spec/setup/kuzuha-reconstruction.md') -Destination (Join-Path $target 'README.md')
Copy-Item -LiteralPath (Join-Path $repoRoot 'spec/feature/kuzuha-reconstruction.md') -Destination (Join-Path $target 'RESEARCH.md')
Copy-Item -LiteralPath (Join-Path $repoRoot 'spec/feature/kuzuha-raymarch.md') -Destination (Join-Path $target 'RAYMARCH.md')
Copy-Item -LiteralPath (Join-Path $repoRoot 'spec/tasks/20260907-kuzuha-pn-demo.md') -Destination (Join-Path $target 'VALIDATION.md')
Copy-Item -LiteralPath (Join-Path $repoRoot 'spec/plan/kuzuha-formula-roadmap.md') -Destination (Join-Path $target 'ROADMAP.md')
# Model and textures stay in the local build package, outside tracked source.
Get-ChildItem -LiteralPath $target -File -Recurse | Where-Object { $_.FullName -ne (Join-Path $target 'manifest.json') } | ForEach-Object {
    [pscustomobject]@{path=$_.FullName.Substring($target.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
} | ConvertTo-Json | ForEach-Object { [System.IO.File]::WriteAllText((Join-Path $target 'manifest.json'), $_, $Utf8NoBom) }
Write-Output "Pictor package: $target"
