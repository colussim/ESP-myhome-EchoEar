![ESP32](https://img.shields.io/badge/ESP32-Embedded-red) ![ESP-IDF](https://img.shields.io/badge/Framework-ESP--IDF-blue) ![Language](https://img.shields.io/badge/Language-C-informational) [![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE) ![Quality Gate](imgs/badges/sonarqube-quality-gate.svg)



#ESP-VoCat AI Voice Satellite


## 🛠️ Hardware

> **Microcontroller:** ESP-VoCat v1.2 ([ESP-VoCat v1.2](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp-vocat/user_guide_v1.2.html))
>

## 💻 Installation ESP-IDF (macOS)

```bash
xcode-select --install

mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git fetch --tags
git checkout v5.4.1
git submodule update --init --recursive
./install.sh esp32s3
. ./export.sh

idf.py --version
ESP-IDF v5.4.1
```

**🏗️ Project creation + adding the Component zigbee**

```bash

idf.py ESP-MYHOME-ECHOEAR
cd ESP-MYHOME-ECHOEAR
idf.py set-target esp32s3

idf.py add-dependency "espressif/minimp3"
idf.py reconfigure

```

## 👩 KIRA Avatar Display (ESP32-S3)

### 👁️ Overview

This project implements a lightweight animated avatar system for an ESP32-S3-based voice satellite.

The avatar, named KIRA, is displayed on a 360 × 360 pixel LCD screen and reacts dynamically during audio playback by animating the mouth.

The goal is to achieve a realistic visual interaction while keeping the system simple, fast, and reliable on embedded hardware.

### 🎭 Avatar Design

•	Base image: 360 × 360 JPEG
•	Optimized to perfectly match the display resolution
•	Always rendered as a static background

To create natural mouth movements:
	1.	The base image was processed using generative AI tools
	2.	Multiple mouth variations were created (speaking, small opening, smile, etc.)
	3.	Each variation was manually refined and aligned to ensure visual consistency

Each mouth variation was manually isolated and exported on a green background (chroma key). This technique ensures accurate background removal and minimizes visual artifacts when compositing the mouth onto the original face.

### 🗣️ Animation Approach

Instead of using video or GIF playback, the system uses a patch-based animation technique:
	•	A fixed region of the face (mouth area) is defined
	•	A set of precomputed mouth frames is stored in memory
	•	During audio playback:
	•	The mouth region is updated in real time
	•	Frames are selected based on audio amplitude

This approach ensures:
	•	Low CPU usage
	•	Minimal memory overhead
	•	High responsiveness

### 🛠️ Asset Processing Pipeline

The asset generation process is handled by Python scripts located in the `scripts/` directory:

- **`convert_kira_color.py`**  
  Composites each mouth variation onto the base face image using chroma key removal.  
  Outputs are saved as PNG files in the `mouth_comp/` directory.

- **`convert_kira_sprites.py`**  
  Converts the composited PNG images into RGB565 raw buffers compatible with the ESP32.  
  The generated `.raw` files are stored in the `data/` directory and loaded at runtime.

  1.	The base face (**`kira_face_360.jpg`**  ) is decoded into RGB565 format
	2.	A mouth background patch (bg_mouth.raw) is used for the neutral state
	3.	Several mouth frames are loaded:
	•	mouth_1_small.raw
	•	mouth_2_medium.raw
	•	mouth_3_smile.raw
	•	mouth_4_small2.raw
	4.	Each frame is stored as RGB565 raw buffers

At runtime:
	•	The mouth background is first restored
	•	A mouth frame is then overlaid depending on the current audio level
	•	The frame buffer is flushed to the display

### 🔊 Audio-Driven Animation

The animation is driven by real-time audio playback:
	•	Audio amplitude is sampled continuously
	•	A smoothing filter is applied to avoid jitter
	•	The amplitude is mapped to discrete mouth states

Example mapping:
	•	Low amplitude → small mouth movement
	•	Medium amplitude → speaking mouth
	•	High amplitude → wider expression (smile/open)

When no audio is playing:
	•	The system displays only the base face (neutral mouth)

### 🚫 Why Not Use GIF or Video?

While GIF or full-frame animation is technically possible, this project intentionally avoids it because:
	•	Higher memory consumption
	•	Increased CPU load
	•	Less flexibility for real-time interaction
	•	Harder synchronization with audio

The patch-based approach provides a better balance between:
	•	Visual quality
	•	Performance
	•	System stability

### ⭐ Key Benefits
• Optimized for ESP32-S3 (PSRAM + SPI Flash)
•	Real-time responsive animation
•	Low resource usage
•	Fully local (no cloud dependency)
•	Easy to extend with new expressions




---

## 📚 References

- [ESP-VoCat v1.2](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp-vocat/user_guide_v1.2.html)
- [User Guide for Built-in Firmware](https://espressif.craft.me/CI2XAhb4Ix7fZk)
- [Examples:](https://github.com/espressif/esp-brookesia/tree/master/products/speaker)
