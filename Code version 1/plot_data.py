import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import glob
import os

# Read data
df = pd.read_csv("sensor_data_log_20250523_094412.csv")
print("\nFirst data:")
print(df.head())

# Plot acceleration
plt.figure(figsize=(15, 8))
plt.plot(df['ax'], label='ax')
plt.plot(df['ay'], label='ay')
plt.plot(df['az'], label='az')
plt.title('Acceleration over time')
plt.xlabel('Sample')
plt.ylabel('Acceleration (m/s²)')
plt.legend()
plt.grid(True)
plt.savefig('acceleration_plot.png')
plt.close()

# Plot gyroscope
plt.figure(figsize=(15, 8))
plt.plot(df['gx'], label='gx')
plt.plot(df['gy'], label='gy')
plt.plot(df['gz'], label='gz')
plt.title('Gyroscope over time')
plt.xlabel('Sample')
plt.ylabel('Angular velocity (deg/s)')
plt.legend()
plt.grid(True)
plt.savefig('gyroscope_plot.png')
plt.close()

# Print basic statistics
print("\nBasic statistics:")
print(df.describe())

print("\nPlots have been created:")
print("1. acceleration_plot.png")
print("2. gyroscope_plot.png")

# Tạo dữ liệu mẫu
x = np.linspace(0, 10, 100)
y = np.sin(x)

# Vẽ đồ thị
plt.figure(figsize=(10, 6))
plt.plot(x, y, label='sin(x)')
plt.title('Đồ thị hàm sin')
plt.xlabel('x')
plt.ylabel('sin(x)')
plt.grid(True)
plt.legend()
plt.show() 