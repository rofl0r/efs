# BBHash sizing benchmark

Comparison of the deterministic BBHash sizing in the parent revision with
collision-dependent sizing and serialized-length metadata in `w`.

## Method

- Used every directory size in `CHECK_N` from the root `Makefile`, with
  `efstest -n N -s 1`.
- The test runner generated a fresh source tree for every round, built an EFS
  image, verified it, and reported its image size before cleanup. A temporary
  runner instrumentation captured that size; the benchmark changes are not
  part of the repository.
- Before the builder read each generated tree and before the reader mapped the
  image, the harness requested file-cache eviction with
  `posix_fadvise(POSIX_FADV_DONTNEED)`. Each round also used fresh temporary
  paths. Kernel-wide cache dropping was unavailable to the unprivileged runner,
  and `posix_fadvise` is advisory.
- For each variant and test set, rounds continued until the timed batch lasted
  at least five seconds. Runtime is the average wall-clock time per complete
  test-runner invocation; image size is the full EFS image size in bytes.
- Variants were built from the deterministic parent revision (`before`) and
  this change (`after`), with the same compiler and test harness. Paired runs
  alternated which variant ran first by test set.

## Results

| Files | Before ms/run | After ms/run | Before rounds | After rounds | Before image bytes | After image bytes |
|------:|--------------:|-------------:|--------------:|-------------:|-------------------:|------------------:|
| 0 | 5.914 | 4.635 | 860 | 1,079 | 212 | 212 |
| 1 | 3.057 | 3.937 | 1,639 | 1,270 | 397 | 397 |
| 2 | 3.655 | 3.904 | 1,368 | 1,281 | 504 | 504 |
| 3 | 3.876 | 5.302 | 1,290 | 956 | 655 | 655 |
| 7 | 11.197 | 7.186 | 448 | 696 | 1,184 | 1,184 |
| 12 | 9.194 | 8.327 | 544 | 601 | 1,578 | 1,578 |
| 50 | 27.178 | 33.418 | 185 | 151 | 6,528 | 6,528 |
| 100 | 48.093 | 48.291 | 105 | 104 | 13,252 | 13,252 |
| 255 | 90.580 | 108.987 | 56 | 47 | 32,910 | 32,902 |
| 256 | 84.388 | 89.721 | 60 | 56 | 33,005 | 32,997 |
| 257 | 104.148 | 102.685 | 51 | 49 | 33,214 | 33,206 |
| 300 | 100.325 | 91.722 | 50 | 55 | 38,218 | 38,218 |
| 500 | 172.473 | 168.982 | 29 | 30 | 62,862 | 62,858 |
| 1,000 | 470.774 | 487.119 | 11 | 11 | 121,770 | 121,794 |
| 2,000 | 914.309 | 1,065.024 | 6 | 5 | 243,453 | 243,409 |

Runtime includes test-data generation, image construction, and full verification,
not only MPH construction. The small test sets are dominated by harness and
process overhead, and the larger-set timings also show run-to-run noise; they
do not establish a consistent runtime change. Image sizes are stable for this
fixed seed and test data. The restored sizing leaves these images unchanged
through 100 files and changes larger images by only a few dozen bytes in either
direction.
