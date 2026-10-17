param([int]$index)  # 0 = 재브(기본 빌드와 같음), 1 = 보브
# 2026-10-18(Docs/설계_보브_보릴_2026-10-18.md) — 브릿지 번호만 다른 빌드. sens_variant.ps1과 같은 방식: 번호만 덮어쓰는 defaults 조각,
# 나머지는 Bridge/sdkconfig.defaults 그대로. 빌드 폴더는 C:\Projects\build_bridge\bridge_<번호>
$env:IDF_TOOLS_PATH="C:\Espressif\tools"
$env:IDF_PATH="C:\esp\v6.0.2\esp-idf"
$env:IDF_PYTHON_ENV_PATH="C:\Espressif\tools\python\v6.0.2\venv"
$env:ESP_IDF_VERSION="6.0.2"
$env:ESP_ROM_ELF_DIR="C:\Espressif\tools\esp-rom-elfs\20241011\"
$env:PATH="C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\python\v6.0.2\venv\Scripts;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\idf-exe\1.0.3;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;$env:PATH"
$proj = "C:\Projects\Ongoing\Bridge"
$sd = "C:\Projects\build_bridge\bridge_$index"
New-Item -ItemType Directory -Force $sd | Out-Null
"CONFIG_BRIDGE_INDEX=$index" | Out-File -Encoding ascii "$sd\index.defaults"
$build = "$sd\build"
if (-not (Test-Path "$build\build.ninja")) {
    cmake -G Ninja -S $proj -B $build -DIDF_TARGET=esp32s3 "-DSDKCONFIG=$sd\sdkconfig" "-DSDKCONFIG_DEFAULTS=$proj\sdkconfig.defaults;$sd\index.defaults" *> "$sd\cmake.log"
    "cmake exit=$LASTEXITCODE"
}
ninja -C $build *> "$sd\ninja.log"
"ninja exit=$LASTEXITCODE"
Get-Content "$sd\ninja.log" | Select-String -Pattern "error:|FAILED|binary size" | Select-Object -First 10 | ForEach-Object { $_.Line }
Select-String -Path "$sd\sdkconfig" -Pattern "^CONFIG_BRIDGE_INDEX" | ForEach-Object { $_.Line }
