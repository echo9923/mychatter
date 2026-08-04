& 'C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -SkipAutomaticLocation
Set-Location D:\codeproject\cpp\llfcchat
cmake --preset windows-ninja -DLLFC_RUN_INTEGRATION_TESTS=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build out/build/windows-ninja --config Release --target im_integration_tests
exit $LASTEXITCODE
