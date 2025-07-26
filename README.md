# Sensor-driven Rehabilitation Assessment

This project investigates a sensor-based approach to evaluating lower-limb rehabilitation performance, particularly focusing on **seated leg press exercises**. The system is designed to capture, process, and analyze data from **inertial measurement units (IMUs)** mounted on exercise equipment in order to assess user movement quality and detect potential ligament injuries or deficiencies in motor control.

Traditional rehabilitation assessments often rely on manual observations or subjective scoring systems performed by therapists. While these methods are valuable, they are prone to inconsistencies and lack the granularity offered by objective measurements. With the growing accessibility of wearable sensors and embedded systems, sensor-driven rehabilitation is becoming a promising field that enables automated, scalable, and reproducible evaluations.

In this project, we build a full data processing pipeline: from **raw sensor signal acquisition**, to **cycle segmentation**, to **feature extraction**, and ultimately **classification** between normal and impaired limb conditions. The goal is to deliver a system that can be deployed in clinical or home-based environments to provide real-time or batch assessments of exercise quality.


## 📁 Project Structure
```
├── .vscode/ # Workspace settings for Visual Studio Code
├── Code version 1/ # Baseline implementation of the processing pipeline
├── Code version 2/ # Improved data collection application
├── Code version 3/ # Advanced version with bias correction 
├── Gen_data/ # Scripts for IMU sensor data stimulating
├── Paper/ # Related manuscripts, figures, and publication materials
├── Visualization/ # Website visualizing signals, cycles, and results
├── data/ # Raw sensor datasets (IMU recordings)
└──  README.md # Project documentation 
```
## 🎯 Objective

The goal of this project is to develop a sensor-based assessment system for lower-limb rehabilitation by analyzing time-series data collected from IMU sensors mounted on seated leg press machines. The system aims to classify and evaluate user performance, distinguish between healthy individuals and those with ligament injuries.

## Technologies

- Python (NumPy, Pandas, SciPy, Matplotlib, etc.), Arduino, HTML5, CSS3, Javascript.
- IMU data processing (accelerometer + gyroscope)
- Signal filtering and cycle segmentation
- Classification and evaluation pipeline
- Data visualization

## 📚 Publication

The `Paper/` folder contains drafts and supporting materials related to the research paper(s) for this project.

## Versions (Arduino code)

- **Code version 1:** Baseline implementation of the data processing pipeline, including filtering, peak detection, and basic segmentation.
- **Code version 2:** Enhanced data collection application with improved handling of sensor input, synchronization, and logging reliability.
- **Code version 3:** Advanced processing to minimize bias and noise in raw IMU signals, focusing on improving the consistency and accuracy of extracted motion features.


## 📩 Contact

For questions or collaboration, feel free to reach out via GitHub or email.

---
