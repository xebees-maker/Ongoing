param([string]$sensor)  # SHT45 / MQ137 / SC05 / SCD41
$env:IDF_TOOLS_PATH="C:\Espressif\tools"
$env:IDF_PATH="C:\esp\v6.0.2\esp-idf"
$env:IDF_PYTHON_ENV_PATH="C:\Espressif\tools\python\v6.0.2\venv"
$env:ESP_IDF_VERSION="6.0.2"
$env:ESP_ROM_ELF_DIR="C:\Espressif\tools\esp-rom-elfs\20241011\"
$env:PATH="C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\python\v6.0.2\venv\Scripts;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\idf-exe\1.0.3;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;$env:PATH"
$proj = "C:\Projects\Ongoing\Sens"
$sd = "C:\Projects\build_sens\sens_$sensor"   # 2026-10-14 - scratchpad 밖으로(재부팅·세션과 무관)
New-Item -ItemType Directory -Force $sd | Out-Null
# 센서 선택만 덮어쓰는 defaults 조각 — 나머지는 sdkconfig.defaults 그대로(09-27 defaults 기준 원칙)
"CONFIG_SENS_SENSOR_$sensor=y" | Out-File -Encoding ascii "$sd\sensor.defaults"
$build = "$sd\build"
if (-not (Test-Path "$build\build.ninja")) {
    cmake -G Ninja -S $proj -B $build -DIDF_TARGET=esp32c3 "-DSDKCONFIG=$sd\sdkconfig" "-DSDKCONFIG_DEFAULTS=$proj\sdkconfig.defaults;$sd\sensor.defaults" *> "$sd\cmake.log"
    "cmake exit=$LASTEXITCODE"
}
ninja -C $build *> "$sd\ninja.log"
"ninja exit=$LASTEXITCODE"
Get-Content "$sd\ninja.log" | Select-String -Pattern "error:|FAILED|binary size" | Select-Object -First 10 | ForEach-Object { $_.Line }
Select-String -Path "$sd\sdkconfig" -Pattern "^CONFIG_SENS_SENSOR_" | ForEach-Object { $_.Line }
