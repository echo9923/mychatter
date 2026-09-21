param(
    [string]$EvidenceDirectory = 'bench_run/results/20260921/reliability'
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
if (Test-Path -LiteralPath $EvidenceDirectory) {
    throw 'Use a new evidence directory to preserve prior test output.'
}
$evidence = New-Item -ItemType Directory -Path $EvidenceDirectory
$runtime = New-Item -ItemType Directory -Path (Join-Path $evidence.FullName 'runtime')
$env:LLFC_TEST_MYSQL_PORT = '13308'
$env:LLFC_TEST_MYSQL_SCHEMA = 'llfc_validation'
$env:LLFC_TEST_MYSQL_PASSWORD = 'llfc-validation-only'
$env:LLFC_TEST_REDIS_PORT = '16380'
$env:LLFC_TEST_REDIS_PASSWORD = 'llfc-validation-only'
$env:QT_QPA_PLATFORM = 'windows'
$env:TEMP = $runtime.FullName
$env:TMP = $runtime.FullName
$ctest = 'C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
python -c "from pathlib import Path; p=Path('out/build/interview-validation/tests/Release/im_integration_tests.exe'); assert b'LLFC_TEST_MYSQL_PORT' in p.read_bytes(), 'Rebuild the integration executable with isolated dependency support'"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$filter = '^(local_store_tests|outbox_dispatcher_tests|resource_upload_tests|im_integration_(dedup|offline|pull-bytes|cross-server|resource-create|resource-offline|resource-upload|resource-resume|resource-idempotent|resource-corrupt))$'
$ErrorActionPreference = 'Continue'
& $ctest --test-dir out/build/interview-validation -C Release --timeout 120 --output-on-failure --output-junit (Join-Path $evidence.FullName 'results.xml') -R $filter *> (Join-Path $evidence.FullName 'ctest.log')
$testExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
Copy-Item -LiteralPath 'out/build/interview-validation/Testing/Temporary/LastTest.log' -Destination (Join-Path $evidence.FullName 'assertions.log')
Get-Content -LiteralPath (Join-Path $evidence.FullName 'ctest.log') -Tail 35
exit $testExit
