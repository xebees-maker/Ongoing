param([string]$proj, [string]$builddir = "")
$env:IDF_TOOLS_PATH="C:\Espressif\tools"
$env:IDF_PATH="C:\esp\v6.0.2\esp-idf"
$env:IDF_PYTHON_ENV_PATH="C:\Espressif\tools\python\v6.0.2\venv"
$env:ESP_IDF_VERSION="6.0.2"
$env:ESP_ROM_ELF_DIR="C:\Espressif\tools\esp-rom-elfs\20241011\"
$env:PATH="C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\python\v6.0.2\venv\Scripts;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\idf-exe\1.0.3;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\esp-clang\esp-20.1.1_20250829\esp-clang\bin;$env:PATH"
if ($builddir -eq "") { $builddir = "C:\Projects\Ongoing\$proj\build" }
$log = "$builddir.log"
ninja -C $builddir *> $log
$code = $LASTEXITCODE
"exit=$code"
Get-Content $log | Select-String -Pattern "error:|warning:|FAILED|binary size" | Select-Object -First 15 | ForEach-Object { $_.Line }
exit $code
