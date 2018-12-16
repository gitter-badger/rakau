[![Build Status](https://img.shields.io/travis/bluescarni/rakau/master.svg?logo=travis&style=for-the-badge)](https://travis-ci.org/bluescarni/rakau)
![language](https://img.shields.io/badge/language-C%2B%2B17-red.svg?style=for-the-badge)
![license](https://img.shields.io/badge/license-MPL2-blue.svg?style=for-the-badge)

rakau
=====

rakau is a C++17 library for the computation of accelerations and potentials in gravitational
[N-body simulations](https://en.wikipedia.org/wiki/N-body_simulation).

The core of the library is a high-performance implementation of the
[Barnes-Hut tree algorithm](https://en.wikipedia.org/wiki/Barnes%E2%80%93Hut_simulation), capable of
taking advantage of modern heterogeneous hardware architectures. Specifically, rakau can run on:

* multicore CPUs, where it takes advantage of both multithreading and vector instructions,
* AMD GPUs, via [ROCm](https://rocm.github.io/),
* Nvidia GPUs, via [CUDA](https://en.wikipedia.org/wiki/CUDA).

On CPUs, multithreaded parallelism is implemented on top of the [TBB library](https://www.threadingbuildingblocks.org/).
Vectorisation is achieved via the [xsimd](https://github.com/QuantStack/xsimd) intrinsics wrapper library
(which means that, e.g., on x86-64 CPUs rakau can use all the available vector extensions, from SSE2 to AVX-512).

At the present time, rakau can accelerate on GPUs the tree traversal part of the Barnes-Hut algorithm.
During tree traversal, the CPU and the GPU can be used concurrently, and the user is free to choose
(depending on the available hardware) how to split the computation between CPU and GPU.

Work is ongoing to accelerate on GPUs other parts of the Barnes-Hut algorithm (most notably, tree
construction).

Performance
-----------

The following table lists the wall runtime, for various hardware configurations, for a complete tree traversal
on a system of 4 million particles distributed according to the [Plummer model](https://en.wikipedia.org/wiki/Plummer_model).
The Barnes-Hut theta parameter is set to 0.75, the computation is done in single-precision.

| Hardware | Type | Runtime |
| :------- | :--- | ------: |
| 2 x Intel Xeon Gold 6148 (AVX-512) | CPU (40 cores + SMT) | 83 ms |
| 2 x Intel Xeon E5-2698 (AVX2) | CPU (40 cores) | 136 ms |
| AMD Ryzen 1700 (AVX2) | CPU (8 cores + SMT) | 592 ms |
| AMD Radeon RX 570 | GPU | 256 ms |
| Nvidia GeForce GTX 1080 Ti | GPU | 140 ms |
| Nvidia V100 | GPU | 95 ms |
| Intel Core i7-3610QM (AVX) | CPU (4 cores + SMT) | 1530 ms |
| Nvidia GeForce GT 650M | GPU | 3135 ms |
| i7-3610QM + GT 650M | GPU+CPU | 1130 ms |

Dependencies
------------

rakau has the following mandatory dependencies:

* the [TBB library](https://www.threadingbuildingblocks.org/),
* the [xsimd library](https://github.com/QuantStack/xsimd),
* the [Boost libraries](https://www.boost.org) (the header-only parts are sufficient,
  apart from the benchmark suite which needs the compiled Boost.Program_options library).

In order to run on AMD GPUs, rakau must be compiled with the ``hcc`` compiler from the
[ROCm toolchain](https://rocm.github.io/). Support for Nvidia GPUs requires the
[CUDA](https://en.wikipedia.org/wiki/CUDA) software stack.