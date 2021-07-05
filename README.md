# AutoTiering
This repo contains the kernel code in the following paper:   
* [Exploring the Design Space of Page Management for Multi-Tiered Memory Systems (ATC'21)](https://www.usenix.org/conference/atc21/presentation/kim-jonghyeon)

## Preliminary
* At least two NUMA node system are needed to define upper-tier and lower-tier node.
* Those different tier memory nodes should have performance gap.
  (e.g., lower-tier node is non-volatile memory, or remote socket memory.)

### Kernel Configuration
* To build _AutoTiering_, you should add options related to _AutoTiering_
    to your kernel configuration file `.config` and load the file by using
    `make menuconfig`

    ``` bash
    # In .config file
    ...
    CONFIG_PAGE_BALANCING=y
    CONFIG_PAGE_BALANCING_DEBUG=y # (optional)
    CONFIG_PAGE_FAULT_PROFILE=y # (optioanl)
    ...
    ```

    ``` bash
    make menuconfig
    ```
  * If you want to trace page access tracking across NUMA nodes, enable `PAGE_BALANCING_DEBUG`.
  * If you want to profile page fault latency including page promotion and demotion, enable `PAGE_FAULT_PROFILE`.

### Kernel Build
``` bash
make -j
make modules_install install
```

## Description
* There are three options for _AutoTiering_, which are CPM, OPM, and Exchange.
* **_AutoTiering-OPM_ includes _AutoTiering-CPM_.** In short, enabling _AutoTiering-OPM_ allow conservative promotion and migration automatically.

  * 000 - Default automatic numa balancing
  * 001 - Enable conservative promotion or migration (_AutoTiering-CPM_)
  * 010 - Enable opportunistic promotion or migration (_AutoTiering-OPM_)
  * 100 - Enable page exchange

## Usage
* It is necessary to enable the original `AutoNUMA balancing` option for adopting _AutoTiering_.
  ``` bash
  # Default AutoNUMA balancing
  sysctl kernel.numa_balancing=1

  # CPM
  sysctl kernel.numa_balancing_extended=1 # [001]

  # CPMX
  sysctl kernel.numa_balancing_extended=5 # [101]

  # OPM
  sysctl kernel.numa_balancing_extended=2 # [010]

  # OPMX
  sysctl kernel.numa_balancing_extended=6 # [110]

  # OPM (Background Demotion)
  sysctl kernel.numa_balancing_extended=2 # [010]
  echo 1 > /sys/kernel/mm/page_balancing/background_demotion
  ```

* `/proc/lapinfo`
  * Show current LAP pages tracked by the system

* In `/sys/kernel/mm/page_balancing` directory,
  * `nr_reserved_pages`
    * Show current reserved page pool that can be promoted
  * `background_demotion`
    * Demote pages using background kernel thread
  * `batch_demotion`
    * Demote pages in batches
  * `skip_lower_tier`
    * Skip access tracking lower-tier memory node
  * `thp_mt_copy`
    * THP copy using multi-thread

