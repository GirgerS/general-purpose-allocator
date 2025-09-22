@echo off

clang -msse4 -maes -O3 examples\windows\heap_test.c -o build\heap_test.exe -I"."
rem clang -msse4 -maes -O0 -g examples\windows\heap_test.c -o build\heap_test.exe -I"."

rem cl /arch:AVX2 /O2 examples\windows\heap_test.c /Fo:build\heap_test.obj /Fe: build\heap_test.exe /I"."
