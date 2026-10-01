# BBHash sizing benchmark

The committed harness measures collision-dependent sizing against the
deterministic sizing from `c0162f4578ddbb066a944d23f0dc35ddc86254b0`.

## Reproduce

Run `./bench_bbhash.sh` for the current source, or pass the deterministic
revision to build that revision's BBHash header against the same current test
runner:

```sh
./bench_bbhash.sh
./bench_bbhash.sh --bbhash-ref c0162f4578ddbb066a944d23f0dc35ddc86254b0
```

`CHECK_N`, `SEED`, and `MIN_BUILD_SECONDS` can be set in the environment. Each
test set runs for at least five wall-clock seconds by default. `--cold-cache`
uses `POSIX_FADV_DONTNEED` advisories for the generated source files and EFS
image. To request a system-wide page-cache drop before each round, run as root
with `--drop-caches`; this runs `sync` and writes `3` to
`/proc/sys/vm/drop_caches`, affecting the whole machine. Cache eviction via
`posix_fadvise` is advisory.

The harness prints average MPH-builder time, complete EFS image-build time,
end-to-end test-runner time, summed MPH-table bytes, full image bytes, and the
MPH share of the image. MPH timing is measured around calls to `efs_mph_build`
only; it excludes filename generation and table serialization into the image.

## Results

Measured with seed 1, default test sets, five seconds per set, and advisory
cache eviction. “Before” is the deterministic revision; “After” is the
collision-dependent sizing in this tree. Timings are average milliseconds per
round; sizes are bytes.

| Files | MPH ms before | MPH ms after | EFS build ms before | EFS build ms after | Table B before | Table B after | Image B before | Image B after | Table % before | Table % after |
|------:|--------------:|-------------:|--------------------:|-------------------:|---------------:|--------------:|---------------:|--------------:|---------------:|--------------:|
| 0 | 0.005 | 0.005 | 0.059 | 0.062 | 48 | 48 | 212 | 212 | 22.642% | 22.642% |
| 1 | 0.006 | 0.006 | 0.058 | 0.058 | 48 | 48 | 397 | 397 | 12.091% | 12.091% |
| 2 | 0.007 | 0.007 | 0.064 | 0.063 | 48 | 48 | 504 | 504 | 9.524% | 9.524% |
| 3 | 0.008 | 0.008 | 0.071 | 0.071 | 48 | 48 | 655 | 655 | 7.328% | 7.328% |
| 7 | 0.014 | 0.014 | 0.128 | 0.127 | 104 | 104 | 1,184 | 1,184 | 8.784% | 8.784% |
| 12 | 0.024 | 0.025 | 0.183 | 0.179 | 104 | 104 | 1,578 | 1,578 | 6.591% | 6.591% |
| 50 | 0.066 | 0.068 | 0.543 | 0.542 | 168 | 168 | 6,528 | 6,528 | 2.574% | 2.574% |
| 100 | 0.131 | 0.142 | 1.308 | 1.340 | 184 | 184 | 13,252 | 13,252 | 1.388% | 1.388% |
| 255 | 0.381 | 0.406 | 6.179 | 5.984 | 352 | 344 | 32,910 | 32,902 | 1.070% | 1.046% |
| 256 | 0.388 | 0.406 | 6.393 | 5.975 | 352 | 344 | 33,005 | 32,997 | 1.067% | 1.043% |
| 257 | 0.380 | 0.401 | 6.342 | 6.086 | 352 | 344 | 33,214 | 33,206 | 1.060% | 1.036% |
| 300 | 0.466 | 0.488 | 8.416 | 8.047 | 360 | 360 | 38,218 | 38,218 | 0.942% | 0.942% |
| 500 | 0.935 | 0.973 | 21.450 | 20.482 | 528 | 524 | 62,862 | 62,858 | 0.840% | 0.834% |
| 1,000 | 2.141 | 2.170 | 79.702 | 75.655 | 764 | 788 | 121,770 | 121,794 | 0.627% | 0.647% |
| 2,000 | 4.934 | 5.099 | 293.254 | 282.351 | 1,348 | 1,304 | 243,453 | 243,409 | 0.554% | 0.536% |

For these fixed generated sets, restored sizing did not yield the predicted
average table-size increase: table sizes were unchanged through 100 files and
differed by at most 24 bytes at larger counts, sometimes shrinking and
sometimes growing. The measured MPH-builder times also show no speed
improvement for deterministic sizing here; timings favor the restored version
by small amounts, with differences too small to draw a general conclusion from
this workload. Larger and more diverse key sets may produce different results.
