# Flashing ESP-myhome-EchoEar with ESP-SR (Wake Word) Support

## Important: Custom Model Partition Handling

This project uses a custom FATFS partition (`model`) to store ESP-SR (speech recognition) models for local wake word detection. By default, the ESP-SR component tries to generate and flash its own `srmodels.bin` file to the same partition, which causes a flash overlap error.

**To avoid this error, you must modify the ESP-SR component's CMake configuration before building and flashing.**

---

## Step-by-Step Flashing Instructions

### 1. Edit ESP-SR CMakeLists.txt

- Open the file:
  
  `managed_components/espressif__esp-sr/CMakeLists.txt`

- Locate the section starting with:
  
  `# Add model partition and flash srmodels.bin`

- **Comment out or remove** the entire block that generates and flashes `srmodels.bin` to the `model` partition. This block typically includes lines with:
  - `add_custom_target(srmodels_bin ...)`
  - `add_dependencies(flash srmodels_bin)`
  - `esptool_py_flash_to_partition(...)`

  > This prevents the ESP-SR component from overwriting your custom FATFS model partition.

### 2. Clean the Build Directory

Run the following commands in your project root:

```sh
idf.py clean
```

### 3. Build the Project

```sh
idf.py build
```

### 4. Flash the Firmware

Connect your ESP32-S3 device and run:

```sh
idf.py -p /dev/cu.usbmodem101 flash
```

---

## Troubleshooting

- **Overlap Error at 0x410000**: If you see an error like `Detected overlap at address: 0x410000 for file: model.bin`, it means the ESP-SR component is still trying to flash `srmodels.bin`. Double-check that you have commented out the correct block in `CMakeLists.txt`.
- **Partition Table**: Ensure your `partitions.csv` defines a `model` partition of type `fat` at the correct offset and size.

---

## Summary
- Always disable the ESP-SR `srmodels.bin` flash logic if you use a custom FATFS model partition.
- Only `model.bin` (generated from your `model/` folder) should be flashed to the `model` partition.

---

For further help, see the project documentation or open an issue.
