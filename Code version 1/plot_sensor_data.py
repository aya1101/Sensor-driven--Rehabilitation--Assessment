import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import glob
import os
from datetime import datetime

def setup_plot_style():
    """Cấu hình style cho đồ thị"""
    plt.style.use('seaborn')
    sns.set_palette('husl')
    plt.rcParams['figure.figsize'] = [12, 6]
    plt.rcParams['font.size'] = 10
    plt.rcParams['font.family'] = 'DejaVu Sans'

def get_latest_csv():
    """Tìm và đọc file CSV mới nhất"""
    csv_files = glob.glob('./sensor_data_log_*.csv')
    if not csv_files:
        raise FileNotFoundError("Không tìm thấy file CSV nào trong thư mục hiện tại!")
    latest_file = max(csv_files, key=os.path.getctime)
    print(f"Đang đọc file: {latest_file}")
    return pd.read_csv(latest_file)

def plot_acceleration(df, save_path='plots'):
    """Vẽ đồ thị gia tốc"""
    plt.figure(figsize=(15, 8))
    plt.plot(df['ax'], label='ax', alpha=0.7)
    plt.plot(df['ay'], label='ay', alpha=0.7)
    plt.plot(df['az'], label='az', alpha=0.7)
    plt.title('Đồ thị gia tốc theo thời gian', pad=20)
    plt.xlabel('Mẫu dữ liệu')
    plt.ylabel('Gia tốc (m/s²)')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(f'{save_path}/acceleration_plot.png', dpi=300, bbox_inches='tight')
    plt.close()

def plot_gyroscope(df, save_path='plots'):
    """Vẽ đồ thị góc"""
    plt.figure(figsize=(15, 8))
    plt.plot(df['gx'], label='gx', alpha=0.7)
    plt.plot(df['gy'], label='gy', alpha=0.7)
    plt.plot(df['gz'], label='gz', alpha=0.7)
    plt.title('Đồ thị góc theo thời gian', pad=20)
    plt.xlabel('Mẫu dữ liệu')
    plt.ylabel('Góc (độ/s)')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(f'{save_path}/gyroscope_plot.png', dpi=300, bbox_inches='tight')
    plt.close()

def plot_distribution(df, save_path='plots'):
    """Vẽ đồ thị phân phối"""
    fig, axes = plt.subplots(2, 3, figsize=(15, 10))
    fig.suptitle('Phân phối dữ liệu gia tốc và góc', y=1.02, fontsize=14)

    # Vẽ histogram cho gia tốc
    sns.histplot(data=df['ax'], ax=axes[0,0], label='ax', kde=True)
    sns.histplot(data=df['ay'], ax=axes[0,1], label='ay', kde=True)
    sns.histplot(data=df['az'], ax=axes[0,2], label='az', kde=True)

    # Vẽ histogram cho góc
    sns.histplot(data=df['gx'], ax=axes[1,0], label='gx', kde=True)
    sns.histplot(data=df['gy'], ax=axes[1,1], label='gy', kde=True)
    sns.histplot(data=df['gz'], ax=axes[1,2], label='gz', kde=True)

    # Thêm tiêu đề cho các subplot
    axes[0,0].set_title('Phân phối ax')
    axes[0,1].set_title('Phân phối ay')
    axes[0,2].set_title('Phân phối az')
    axes[1,0].set_title('Phân phối gx')
    axes[1,1].set_title('Phân phối gy')
    axes[1,2].set_title('Phân phối gz')

    plt.tight_layout()
    plt.savefig(f'{save_path}/distribution_plot.png', dpi=300, bbox_inches='tight')
    plt.close()

def plot_correlation(df, save_path='plots'):
    """Vẽ ma trận tương quan"""
    plt.figure(figsize=(12, 10))
    correlation_matrix = df[['ax', 'ay', 'az', 'gx', 'gy', 'gz']].corr()
    sns.heatmap(correlation_matrix, annot=True, cmap='coolwarm', center=0)
    plt.title('Ma trận tương quan giữa các biến', pad=20)
    plt.tight_layout()
    plt.savefig(f'{save_path}/correlation_plot.png', dpi=300, bbox_inches='tight')
    plt.close()

def main():
    # Tạo thư mục plots nếu chưa tồn tại
    save_path = 'plots'
    if not os.path.exists(save_path):
        os.makedirs(save_path)

    # Cấu hình style
    setup_plot_style()

    try:
        # Đọc dữ liệu
        df = get_latest_csv()
        print("\nDữ liệu đầu tiên:")
        print(df.head())

        # Vẽ các đồ thị
        plot_acceleration(df, save_path)
        plot_gyroscope(df, save_path)
        plot_distribution(df, save_path)
        plot_correlation(df, save_path)

        # In thống kê cơ bản
        print("\nThống kê cơ bản:")
        print(df.describe())

        print(f"\nĐã tạo xong các đồ thị trong thư mục '{save_path}':")
        print("1. acceleration_plot.png")
        print("2. gyroscope_plot.png")
        print("3. distribution_plot.png")
        print("4. correlation_plot.png")

    except Exception as e:
        print(f"\nLỗi: {str(e)}")

if __name__ == "__main__":
    main() 