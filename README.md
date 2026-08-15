# Simple Open EtherCAT Master Library (Xenomai 4 EVL Fork)

* Copyright (C) 2005-2025 Speciaal Machinefabriek Ketels v.o.f.
* Copyright (C) 2005-2025 Arthur Ketels
* Copyright (C) 2009-2025 RT-Labs AB, Sweden
* *Xenomai 4 (EVL) support added.*

SOEM (Simple Open EtherCAT Master) is a software library for developing EtherCAT MainDevices.

This library is specifically designed for real-time communication in embedded systems. Its lightweight architecture minimizes resource consumption, making it suitable for environments with limited resources.


## Xenomai 4 (EVL) Out-of-Band Support

This fork introduces native support for Xenomai 4 (EVL) out-of-band execution.
SOEM has been modified to leverage the **EVL real-time core** for deterministic and low-latency EtherCAT communication.

The advantages of using Xenomai 4 (EVL) with SOEM include:
* *Hard Real-Time Determinism*: Bypasses the standard Linux networking stack and scheduler.
* *Low Jitter*: EtherCAT cyclic tasks and socket I/O run entirely in the EVL out-of-band context.
* *Automated Ingress Routing*: Includes an eBPF filter to seamlessly divert EtherCAT frames (`ETH_P_ECAT`) into the out-of-band network stack.

To compile SOEM with EVL support, configure CMake with the following flag:

```bash
cmake -B build -DUSE_XENOMAI_EVL=ON
```

> [!Note]
> This feature requires a Xenomai 4 (EVL) patched Linux kernel to function correctly.
> See `INSTALL.md` for instructions on how to set up your machine.


## Documentation

See https://docs.rt-labs.com/soem

## Contributions

Contributions are welcome. If you want to contribute you will need to sign a Contributor License Agreement and send it to us either by e-mail or by physical mail. More information is available on [https://rt-labs.com/contribution](https://rt-labs.com/contribution).