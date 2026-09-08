86Box tests and benchmarks
=========================
Last updated: 2026-09-08

## Overview

This directory, `tests/`, holds C and C++ tests and benchmarks organized to mirror `src/`. For example, for the Mitsumi emulated device:

Driver:  
`src/cdrom/cdrom_mitsumi.c`

Tests:  
`tests/cdrom/cdrom_mitsumi_test.cpp`
`tests/cdrom/cdrom_mitsumi_benchmark.cpp`

Try to match your code's filename and append the type of test it is.

## Summary of current tests

# Mitsumi

The Mitsumi tests exercise the device implementation in isolation using mocked CD-ROM, DMA, interrupt and timer dependencies. They are device-level unit tests, not full-emulator or guest-driver integration tests. The benchmark measures performance and is not a correctness test.

Read tests advance the device's registered timer callback deterministically; issuing a read command alone does not complete a sector transfer. The GET_STAT regression checks that a pending media-change indication reaches the returned status byte rather than being consumed twice. These tests do not establish physical-drive timing or reset/insertion change-latch lifetime.

Enable `BUILD_TESTING` with GoogleTest available, then build `mitsumi_cdrom_tests` and run `ctest --test-dir <build-directory> --output-on-failure`. Enable `BUILD_BENCHMARKS` with Google Benchmark available to build `mitsumi_cdrom_benchmarks`.
