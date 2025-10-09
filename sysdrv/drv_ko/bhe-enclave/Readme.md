# BHE Enclave Kernel Driver

## Overview
The BHE Enclave kernel driver is a Linux kernel module designed to interface with a UART-based enclave device on embedded platforms, such as the Luckfox Pico and Raspberry Pi 5. It provides a communication bridge between userspace applications and a hardware enclave connected via a serial (UART) port. The driver exposes a misc character device (`/dev/enclave`) for reading and writing data, and creates sysfs entries to monitor the enclave's state and version. The driver is compatible with Linux kernel version 5.10.160 and uses the serdev (serial device) framework for UART communication.

## Features
- **UART Communication**: Interfaces with a hardware enclave over a UART port, supporting configurable baud rates (default: 9600).
- **Misc Device Interface**: Provides a character device (`/dev/enclave`) for userspace applications to send and receive data to/from the enclave.
- **Sysfs Integration**: Exposes read-only attributes (`root_state` and `version`) under `/sys/class/enclave/` to monitor enclave status.
- **Packet Processing**: Parses incoming UART data packets with a specific signature (`0xBADF00D`) and handles state update packets (type `0x0004`) to update `root_state` and `version`.
- **Buffered I/O**: Uses a circular buffer for UART data to handle asynchronous communication, with support for non-blocking reads.
- **Device Tree Support**: Matches devices with the compatible string `bhe,card-enclave` in the device tree.

## Dependencies
- **Kernel Configuration**:
  - `CONFIG_SERIAL_DEV_BUS`: Must be enabled (`y` or `m`) in the kernel configuration to support the serdev framework.
  - Standard kernel headers: `linux/module.h`, `linux/device.h`, `linux/serdev.h`, `linux/miscdevice.h`, etc.
- **Hardware**: A UART-capable device (e.g., Luckfox Pico or Raspberry Pi 5) with a connected enclave hardware module.
- **Cross-Compiler**: For Luckfox Pico, use `arm-rockchip830-linux-uclibcgnueabihf-`. For Raspberry Pi 5, use an appropriate ARM cross-compiler (e.g., `arm-linux-gnueabihf-`).

## UART Port Configuration
The driver communicates over a UART port managed by the serdev framework. The UART port is specified in the device tree with the compatible string `bhe,card-enclave`. For example:

```dts
&uart0 {
    status = "okay";
    enclave {
        compatible = "bhe,card-enclave";
    };
};
```

- **Baud Rate**: Configurable via the module parameter `baud_rate` (default: 9600). Set at module load time, e.g.:
  ```bash
  insmod bhe-enclave.ko baud_rate=115200
  ```
- **Data Format**: The driver expects packets with an 8-byte header:
  - Bytes 0-3: Signature (`0xBADF00D`, little-endian).
  - Bytes 4-5: Packet type (e.g., `0x0004` for state updates, little-endian).
  - Bytes 6-7: Payload size (little-endian).
  - Payload: Variable size, following the header.
- **State Update Packet**: For packet type `0x0004`, the payload must be at least 2 bytes:
  - Byte 0: `root_state` (unsigned char).
  - Byte 1: `version` (unsigned char).

## Device Interface
- **Misc Device**: `/dev/enclave`
  - **Read**: Reads data from the UART buffer. Supports blocking and non-blocking modes (returns `-EAGAIN` if no data is available in non-blocking mode).
  - **Write**: Sends data to the UART port. Supports dynamic buffer allocation for large writes (stack buffer for up to 128 bytes, heap for larger).
  - **Permissions**: World-readable and writable (`0666`).
- **Sysfs Entries**: Under `/sys/class/enclave/enclave-<device_name>/`
  - `root_state`: Read-only, displays the current enclave state (updated by packet type `0x0004`).
  - `version`: Read-only, displays the enclave version (updated by packet type `0x0004`).
  Example:
  ```bash
  cat /sys/class/enclave/enclave-uart0/root_state
  cat /sys/class/enclave/enclave-uart0/version
  ```

## Buffer Management
- **Receive Buffer**: A 2048-byte buffer (`rx_buffer`) stores incoming UART data. If the buffer overflows, data is discarded, and a warning is logged.
- **Circular Buffer**: A 2048-byte circular buffer (`uart_buffer`) stores processed packet data for userspace reads. Overflows are handled by dropping the oldest data.
- **Synchronization**: Uses a mutex (`buffer_lock`) to protect buffer access and a wait queue (`uart_wait_queue`) for blocking reads.

## Building the Driver
### Prerequisites
- Kernel source tree: Located at `/src/bhe-luckfox-pico/sysdrv/source/kernel` for Luckfox Pico.
- Cross-compiler: `arm-rockchip830-linux-uclibcgnueabihf-` for Luckfox Pico or appropriate ARM compiler for other platforms.
- Device tree: Ensure the `bhe,card-enclave` compatible string is present in the device tree.

### Build Command
```bash
make ARCH=arm CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf- -C /src/bhe-luckfox-pico/sysdrv/source/kernel M=/src/bhe-luckfox-pico/sysdrv/drv_ko/bhe-enclave modules -j12
```
This generates `bhe-enclave.ko` in the module directory.

### Installation
1. Copy the module to the target system:
   ```bash
   cp /src/bhe-luckfox-pico/sysdrv/drv_ko/bhe-enclave/bhe-enclave.ko /lib/modules/5.10.160/kernel/drivers/
   ```
2. Update module dependencies:
   ```bash
   depmod -a 5.10.160
   ```
3. Load the module:
   ```bash
   insmod bhe-enclave.ko [baud_rate=<value>]
   ```

## Usage
1. **Load the Module**:
   ```bash
   insmod bhe-enclave.ko
   ```
   Optionally, set the baud rate:
   ```bash
   insmod bhe-enclave.ko baud_rate=115200
   ```
2. **Verify Device**:
   - Check `dmesg` for probe messages:
     ```
     UART Probe Called for uart0
     UART device probed successfully
     ```
   - Confirm the misc device exists:
     ```bash
     ls /dev/enclave
     ```
   - Check sysfs entries:
     ```bash
     ls /sys/class/enclave/
     ```
3. **Read/Write Data**:
   - Write to the enclave:
     ```bash
     echo -n "data" > /dev/enclave
     ```
   - Read from the enclave:
     ```bash
     cat /dev/enclave
     ```
   - Non-blocking read:
     ```bash
     cat /dev/enclave < /dev/null
     ```
4. **Monitor State**:
   - Read enclave state and version:
     ```bash
     cat /sys/class/enclave/enclave-uart0/root_state
     cat /sys/class/enclave/enclave-uart0/version
     ```

## Debugging
- **Logs**: Check `dmesg` for driver messages, including:
  - Probe success/failure.
  - Packet processing errors (e.g., invalid signature, buffer overflow).
  - UART write wakeup events.
- **Common Issues**:
  - **Module Fails to Load**: Ensure `CONFIG_SERIAL_DEV_BUS` is enabled and the device tree includes the `bhe,card-enclave` compatible string.
  - **No Data Received**: Verify the UART connection and baud rate match the enclave hardware.
  - **Sysfs Errors**: Check if `/sys/class/enclave/` exists and permissions are correct.
- **Runtime Errors**: If the driver probes but fails to communicate, verify the UART port configuration and enclave hardware functionality.

## Porting Notes
- **From Raspberry Pi 5 to Luckfox Pico**:
  - The driver was originally developed for Debian on Raspberry Pi 5 and ported to Luckfox Pico (kernel 5.10.160).
  - Key changes for Luckfox Pico:
    - Ensured C90 compliance (variable declarations before code).
    - Updated `class_create` to use two arguments (`THIS_MODULE`, class name).
    - Replaced `module_serdev_device_driver` with explicit `driver_register`/`driver_unregister` due to potential macro absence.
  - Ensure the cross-compiler and kernel configuration match the target platform.
- **Device Tree**: Update the device tree to match the UART port used on the target platform (e.g., `uart0` on Luckfox Pico).

## Module Parameters
- `baud_rate` (int, default: 9600, permissions: 0644)
  - Sets the UART baud rate.
  - Example: `insmod bhe-enclave.ko baud_rate=115200`

## License
- **License**: GPL
- **Author**: Matias Sebastian Soler <matias.s.soler@gmail.com>
- **Version**: 0.1
- **Description**: Hello World (UART-based enclave driver)
