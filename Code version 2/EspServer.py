import serial
import serial.tools.list_ports
import time
import json
import re
import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext, filedialog
import threading
import collections
import os
import datetime

# --- Matplotlib for plotting ---
import matplotlib.pyplot as plt
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
from matplotlib.animation import FuncAnimation
import numpy as np

# --- General Configuration ---
BAUD_RATE = 115200
MAX_PLOT_POINTS = 200
MAX_LOG_LINES = 100 # Increased for better log visibility

# --- Global variables for connection and data management ---
ser = None
running = False
connected_nodes_data = {} # Stores latest data for each node
serial_thread = None
last_summary_time = time.time()
# Thêm biến toàn cục
recording_thread = None
recording_thread_stop = threading.Event()


# --- Global variables for file recording ---
is_recording = False
output_directory = os.path.dirname(os.path.abspath(__file__)) # Default to script directory
active_recording_file_handles = {} # Dictionary to store open file handles for continuous recording
recording_queue = collections.deque() # Separate queue for data recording

# --- Data for plotting ---
plot_data_buffers = {} # Stores historical data for plotting, keyed by node ID
data_queue = collections.deque() # Queue to transfer new data from serial thread to main/plot thread

# --- Matplotlib objects ---
fig_acc = None
ax_acc = None
line_ax, line_ay, line_az = None, None, None
fig_gyro = None
ax_gyro = None
line_gx, line_gy, line_gz = None, None, None
ani_acc = None
ani_gyro = None

# --- Global variables for configuration management (saving state) ---
CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "esp_server_config.txt")

# --- Node selection for plotting ---
selected_node_for_plot = None # Global variable to hold the ID of the node currently selected for plotting

# --- Timestamp Conversion Function ---
def format_microseconds_to_mmss_us(microseconds):
    if not isinstance(microseconds, (int, float)) or microseconds < 0:
        return "Invalid_TS"

    total_seconds = int(microseconds // 1_000_000)
    us_part = int(microseconds % 1_000_000)
    minutes = (total_seconds // 60) % 60
    seconds = total_seconds % 60

    return f"{minutes:02}:{seconds:02}:{us_part:06}"

# This function is no longer used directly but can be useful for re-parsing
def parse_mmss_us_to_microseconds(timestamp_str):
    try:
        parts = timestamp_str.split(':')
        if len(parts) == 3:
            minutes = int(parts[0])
            seconds = int(parts[1])
            microseconds_part = int(parts[2])
            total_microseconds = (minutes * 60 * 1_000_000) + (seconds * 1_000_000) + microseconds_part
            return total_microseconds
    except ValueError:
        pass
    return 0

# --- Functions to parse different types of data from Serial ---
def parse_sensor_data(line):
    # --- Case 1: Dữ liệu dạng JSON (cũ)
    json_str_start_idx = line.find("Processed UDP from Queue: ")
    if json_str_start_idx != -1:
        json_candidate = line[json_str_start_idx + len("Processed UDP from Queue: "):].strip()
    else:
        json_candidate = line.strip()

    if json_candidate.startswith("{") and json_candidate.endswith("}"):
        try:
            data = json.loads(json_candidate)
            required_keys = ["id", "ax", "ay", "az", "gx", "gy", "gz", "ts"]
            if all(k in data for k in required_keys):
                return {
                    "id": data["id"],
                    "ax": float(data["ax"]),
                    "ay": float(data["ay"]),
                    "az": float(data["az"]),
                    "gx": float(data["gx"]),
                    "gy": float(data["gy"]),
                    "gz": float(data["gz"]),
                    "ts": int(data["ts"]) * 1000  # milliseconds -> microseconds
                }
        except (json.JSONDecodeError, ValueError, TypeError):
            return None

    # --- Case 2: Dữ liệu dạng text phân tách bằng `:`
    if line.startswith("DATA:"):
        try:
            parts = line.strip().split(":")
            if len(parts) >= 10:
                return {
                    "id": parts[1],
                    "ts": int(parts[3]) * 1000,  # convert ms -> µs
                    "ax": float(parts[4]),
                    "ay": float(parts[5]),
                    "az": float(parts[6]),
                    "gx": float(parts[7]),
                    "gy": float(parts[8]),
                    "gz": float(parts[9])
                }
        except (ValueError, IndexError):
            return None

    return None


def parse_server_uptime(line):
    if "Server Uptime:" in line and "seconds" in line:
        try:
            match = re.search(r'Server Uptime: (\d+) seconds', line)
            if match:
                return int(match.group(1))
        except ValueError:
            pass
    return None

def is_separator_line(line):
    return line.strip() == "--------------------------------------------------" or \
           line.strip() == "-------------------------------------------------"

# --- GUI Functions ---
def update_com_ports():
    ports = serial.tools.list_ports.comports()
    com_ports = [port.device for port in ports]
    com_port_combobox['values'] = com_ports
    if com_ports:
        com_port_combobox.set(com_ports[0])
    else:
        com_port_combobox.set("Không tìm thấy cổng")

def start_serial_read_thread():
    global ser, running, serial_thread, last_summary_time, ani_acc, ani_gyro, selected_node_for_plot
    selected_port = com_port_combobox.get()

    if not selected_port or selected_port == "Không tìm thấy cổng":
        messagebox.showerror("Lỗi kết nối", "Vui lòng chọn một cổng COM hợp lệ.")
        return

    if running:
        messagebox.showinfo("Thông báo", "Đang đọc dữ liệu. Vui lòng nhấn 'Ngắt kết nối' trước khi bắt đầu lại.")
        return

    try:
        ser = serial.Serial(selected_port, BAUD_RATE, timeout=0.1)
        running = True
        connect_button.config(text="Đang kết nối...", state=tk.DISABLED)
        disconnect_button.config(state=tk.NORMAL)
        toggle_recording_buttons_state()
        status_label.config(text=f"Đã kết nối tới {selected_port}", style="Green.TLabel")
        log_text.insert(tk.END, f"[INFO] Đã mở cổng Serial {selected_port} với tốc độ {BAUD_RATE} bps.\n")
        log_text.insert(tk.END, "[INFO] Đang chờ dữ liệu từ ESP32...\n")
        log_text.insert(tk.END, "[INFO] Dữ liệu RAW từ Serial sẽ hiển thị ngay khi nhận được.\n")
        log_text.insert(tk.END, "[INFO] Nhấn 'Ngắt kết nối' hoặc đóng cửa sổ để dừng chương trình.\n\n")
        log_text.see(tk.END)

        last_summary_time = time.time()
        serial_thread = threading.Thread(target=read_serial_data_loop, daemon=True)
        serial_thread.start()

        # Initialize animations for plots (only run when connected)
        ani_acc = FuncAnimation(fig_acc, update_acc_plot, interval=100, blit=False)
        ani_gyro = FuncAnimation(fig_gyro, update_gyro_plot, interval=100, blit=False)
        fig_acc_canvas.draw()
        fig_gyro_canvas.draw()

        # Clear node selection and data buffers on new connection
        selected_node_for_plot = None
        connected_nodes_data.clear()
        plot_data_buffers.clear()
        update_node_selection_combobox()
        root.after(50, process_queue_loop)

 # Clear combobox initially

    except serial.SerialException as e:
        messagebox.showerror("Lỗi kết nối", f"Không thể mở cổng {selected_port}:\n{e}\nVui lòng kiểm tra:\n  - Cổng COM có đang bị sử dụng bởi chương trình khác không? (VD: Arduino IDE Serial Monitor)\n  - ESP32 đã kết nối chưa và driver đã cài đặt?")
        status_label.config(text="Không kết nối", style="Red.TLabel")
        connect_button.config(text="Kết nối", state=tk.NORMAL)
        disconnect_button.config(state=tk.DISABLED)
        toggle_recording_buttons_state()
    except Exception as e:
        messagebox.showerror("Lỗi hệ thống", f"Đã xảy ra lỗi không mong muốn khi bắt đầu kết nối: {e}")
        status_label.config(text="Lỗi hệ thống", style="Red.TLabel")
        connect_button.config(text="Kết nối", state=tk.NORMAL)
        disconnect_button.config(state=tk.DISABLED)
        toggle_recording_buttons_state()

def stop_serial_read():
    global running, ser, serial_thread
    global ani_acc, ani_gyro
    global is_recording, selected_node_for_plot
    global connected_nodes_data, plot_data_buffers

    if not running:
        log_text.insert(tk.END, "[INFO] Chương trình đã dừng hoặc chưa kết nối.\n")
        log_text.see(tk.END)
        return

    # Dừng ghi dữ liệu nếu đang ghi
    if is_recording:
        stop_recording_data()
        messagebox.showinfo("Thông báo", "Quá trình ghi dữ liệu liên tục đã dừng.")

    running = False  # Đặt sau khi stop_recording để đảm bảo nút được xử lý đúng

    # Ngắt thread đọc serial
    if serial_thread and serial_thread.is_alive():
        serial_thread.join(timeout=1.0)

    # Đóng serial port nếu còn mở
    if ser and ser.is_open:
        ser.close()
        log_text.insert(tk.END, "[INFO] Đã đóng cổng Serial.\n")
        log_text.see(tk.END)

    # Cập nhật giao diện
    status_label.config(text="Đã ngắt kết nối", style="Orange.TLabel")
    connect_button.config(text="Kết nối", state=tk.NORMAL)
    disconnect_button.config(state=tk.DISABLED)
    toggle_recording_buttons_state()

    # Dừng animation nếu đang chạy
    if ani_acc:
        ani_acc.event_source.stop()
        ani_acc = None
    if ani_gyro:
        ani_gyro.event_source.stop()
        ani_gyro = None

    # Dọn dữ liệu buffer và hiển thị
    connected_nodes_data.clear()
    plot_data_buffers.clear()
    selected_node_for_plot = None
    update_data_display()
    update_node_selection_combobox()

    # Reset biểu đồ
    if ax_acc:
        ax_acc.clear()
        ax_acc.set_title("Gia tốc (Accelerometer) - Waiting for data...")
        ax_acc.set_ylabel("Giá trị")
        ax_acc.set_xlabel("Thời gian (Điểm)")
        global line_ax, line_ay, line_az
        line_ax, = ax_acc.plot([], [], label='Ax')
        line_ay, = ax_acc.plot([], [], label='Ay')
        line_az, = ax_acc.plot([], [], label='Az')
        ax_acc.legend()
        ax_acc.set_ylim(-9000, 9000)
        ax_acc.set_xlim(0, MAX_PLOT_POINTS)
        fig_acc_canvas.draw_idle()

    if ax_gyro:
        ax_gyro.clear()
        ax_gyro.set_title("Vận tốc góc (Gyroscope) - Waiting for data...")
        ax_gyro.set_ylabel("Giá trị")
        ax_gyro.set_xlabel("Thời gian (Điểm)")
        global line_gx, line_gy, line_gz
        line_gx, = ax_gyro.plot([], [], label='Gx')
        line_gy, = ax_gyro.plot([], [], label='Gy')
        line_gz, = ax_gyro.plot([], [], label='Gz')
        ax_gyro.legend()
        ax_gyro.set_ylim(-35000, 35000)
        ax_gyro.set_xlim(0, MAX_PLOT_POINTS)
        fig_gyro_canvas.draw_idle()

    log_text.insert(tk.END, "[INFO] Chương trình đã dừng đọc dữ liệu Serial.\n")
    log_text.see(tk.END)


def read_serial_data_loop():
    global ser, running, last_summary_time, data_queue
    while running:
        if ser and ser.is_open:
            try:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    data_queue.append(line)
                    root.after(0, process_queue_data)

                current_time = time.time()
                if current_time - last_summary_time > 5:
                    if connected_nodes_data:
                        root.after(0, update_summary_display)
                    last_summary_time = current_time

            except serial.SerialException as e:
                root.after(0, lambda: log_error_and_stop(f"Lỗi Serial trong khi đọc: {e}. Vui lòng kiểm tra kết nối."))
                break
            except Exception as e:
                root.after(0, lambda: log_error_and_stop(f"Lỗi không xác định trong khi đọc: {e}"))
                break
        else:
            break
        time.sleep(0.01)

def process_queue_data():
    # Xử lý các dòng dữ liệu Serial đã nhận
    while data_queue:
        try:
            line = data_queue.popleft()
            process_serial_line_gui(line)
        except Exception as e:
            log_text.insert(tk.END, f"[ERROR] Lỗi khi xử lý dòng dữ liệu: {e}\n")
            log_text.see(tk.END)

def process_queue_loop():
    process_queue_data()
    root.after(50, process_queue_loop)  # Kiểm tra mỗi 50ms (phù hợp với 10Hz = 100ms)


def parse_handshake_data(line):
    """
    Trích xuất node_id từ các thông điệp bắt tay (handshake).
    Có thể là:
    - "Received HELLO from Node ID: Sensor_1"
    - "Sent WELCOME to IP:192.168.4.2 -> WELCOME:Sensor_1:..."
    """
    if "Received HELLO from Node ID:" in line:
        try:
            return line.split("Received HELLO from Node ID:")[1].strip()
        except IndexError:
            return None

    elif "WELCOME:" in line:
        try:
            parts = line.split("WELCOME:")[1].split(":")
            if parts:
                return parts[0].strip()  # Node ID ở đầu
        except IndexError:
            return None

    return None


def parse_wifi_client_count(line):
    """
    Trích xuất số lượng client WiFi từ chuỗi debug ESP32.
    Ví dụ chuỗi: "DEBUG: WiFi SoftAP Connected Clients (WiFi layer): --- 3 clients"
    """
    keyword = "DEBUG: WiFi SoftAP Connected Clients"
    if keyword in line:
        try:
            # Lấy phần sau dấu ":" cuối cùng, tách lấy số đầu tiên
            tail = line.split(":")[-1]
            count_str = tail.strip().split()[0]
            return int(count_str)
        except (ValueError, IndexError):
            return None
    return None

def process_serial_line_gui(line):
    global selected_node_for_plot

    # Clear old lines if exceeding limit
    current_lines = int(log_text.index('end-1c').split('.')[0])
    if current_lines > MAX_LOG_LINES:
        log_text.delete(1.0, float(current_lines - MAX_LOG_LINES + 1))

    log_text.insert(tk.END, f"RAW: {line}\n")
    log_text.see(tk.END)

    if is_separator_line(line):
        pass
    else:
        node_id_handshake = parse_handshake_data(line)
        if node_id_handshake:
            log_text.insert(tk.END, f"  -> HANDSHAKE: Node ID '{node_id_handshake}' connected.\n")
            log_text.see(tk.END)
            if node_id_handshake not in connected_nodes_data:
                connected_nodes_data[node_id_handshake] = {
                    "ax": 0, "ay": 0, "az": 0,
                    "gx": 0, "gy": 0, "gz": 0,
                    "ts_us": 0,
                    "ts_formatted": "00:00:000000",
                    "status": "Connected (Handshake)"
                }
                # If no node is selected, select this new one
                if selected_node_for_plot is None:
                    selected_node_for_plot = node_id_handshake
                    node_select_combobox.set(selected_node_for_plot) # Update combobox display
                    update_node_selection_combobox() # Ensure it's in the list
                    fig_acc_canvas.draw_idle()
                    fig_gyro_canvas.draw_idle()

            else:
                connected_nodes_data[node_id_handshake]["status"] = "Connected (Handshake)"
            update_data_display()
            update_node_selection_combobox() # Update combobox with new nodes

        wifi_client_count = parse_wifi_client_count(line)
        if wifi_client_count is not None:
            log_text.insert(tk.END, f"  -> DEBUG: Số lượng client WiFi kết nối: {wifi_client_count}\n")
            log_text.see(tk.END)

        server_uptime = parse_server_uptime(line)
        if server_uptime is not None:
            log_text.insert(tk.END, f"  -> INFO: Server Uptime: {server_uptime} seconds\n")
            log_text.see(tk.END)

        sensor_data = parse_sensor_data(line)
        if not sensor_data:
            log_text.insert(tk.END, f"[WARN] Không parse được sensor data từ dòng:\n  {line}\n")
            log_text.see(tk.END)
            return

        if "id" not in sensor_data:
            log_text.insert(tk.END, f"[ERROR] Dữ liệu thiếu 'id': {sensor_data}\n")
            log_text.see(tk.END)
            return

        if sensor_data:
            node_id = sensor_data["id"]
            ts_microseconds = sensor_data.pop('ts')

            ts_formatted_str = format_microseconds_to_mmss_us(ts_microseconds)

            node_current_data = {
                'id': node_id,
                'ax': sensor_data['ax'], 'ay': sensor_data['ay'], 'az': sensor_data['az'],
                'gx': sensor_data['gx'], 'gy': sensor_data['gy'], 'gz': sensor_data['gz'],
                'ts_us': ts_microseconds,
                'ts_formatted': ts_formatted_str,
                "status": "Active"
            }
            connected_nodes_data[node_id] = node_current_data

            # Initialize plot buffer for new nodes
            if node_id not in plot_data_buffers:
                plot_data_buffers[node_id] = {
                    "ax": collections.deque(maxlen=MAX_PLOT_POINTS),
                    "ay": collections.deque(maxlen=MAX_PLOT_POINTS),
                    "az": collections.deque(maxlen=MAX_PLOT_POINTS),
                    "gx": collections.deque(maxlen=MAX_PLOT_POINTS),
                    "gy": collections.deque(maxlen=MAX_PLOT_POINTS),
                    "gz": collections.deque(maxlen=MAX_PLOT_POINTS),
                }

            # Append all sensor data to their respective node's buffer
            plot_data_buffers[node_id]["ax"].append(sensor_data['ax'])
            plot_data_buffers[node_id]["ay"].append(sensor_data['ay'])
            plot_data_buffers[node_id]["az"].append(sensor_data['az'])
            plot_data_buffers[node_id]["gx"].append(sensor_data['gx'])
            plot_data_buffers[node_id]["gy"].append(sensor_data['gy'])
            plot_data_buffers[node_id]["gz"].append(sensor_data['gz'])

            # If no node is selected for plot yet, select this one
            if selected_node_for_plot is None:
                selected_node_for_plot = node_id
                node_select_combobox.set(selected_node_for_plot) # Update combobox display
                update_node_selection_combobox() # Ensure it's in the list
                fig_acc_canvas.draw_idle()
                fig_gyro_canvas.draw_idle()

            # Add data to recording queue if recording is active
            if is_recording:
                recording_queue.append(node_current_data)

            update_data_display()
            toggle_save_button_state() # Activate Save button if data is present
            update_node_selection_combobox() # Update combobox with new nodes

def update_data_display():
    print(">>> Updating data display...")
    for item in data_tree.get_children():
        data_tree.delete(item)

    for node_id, data in connected_nodes_data.items():
        print(f">>> Inserting node: {node_id} | data = {data}")
        data_tree.insert("", tk.END, iid=node_id, values=(
            node_id,
            data.get('status', 'Unknown'),
            f"{data['ax']:.2f}", f"{data['ay']:.2f}", f"{data['az']:.2f}",
            f"{data['gx']:.2f}", f"{data['gy']:.2f}", f"{data['gz']:.2f}",
            data['ts_formatted'],
            data['ts_us']
        ))

def update_summary_display():
    # Summary data is now primarily in the Treeview and can be written to file.
    pass

def log_error_and_stop(message):
    log_text.insert(tk.END, f"[ERROR] {message}\n")
    log_text.see(tk.END)
    stop_serial_read()

def on_closing():
    if messagebox.askokcancel("Thoát", "Bạn có chắc chắn muốn thoát không?"):
        stop_serial_read()
        root.destroy()

# --- Matplotlib Plotting Functions ---
def setup_plots():
    global fig_acc, ax_acc, line_ax, line_ay, line_az, fig_gyro, ax_gyro, line_gx, line_gy, line_gz

    # Figure for Accelerometer
    fig_acc = plt.Figure(figsize=(6, 4), dpi=100)
    ax_acc = fig_acc.add_subplot(111)
    ax_acc.set_title("Gia tốc (Accelerometer) - Waiting for data...")
    ax_acc.set_ylabel("Giá trị")
    ax_acc.set_xlabel("Thời gian (Điểm)")
    line_ax, = ax_acc.plot([], [], label='Ax')
    line_ay, = ax_acc.plot([], [], label='Ay')
    line_az, = ax_acc.plot([], [], label='Az')
    ax_acc.legend()
    ax_acc.set_ylim(-9000, 9000)
    ax_acc.set_xlim(0, MAX_PLOT_POINTS)

    # Figure for Gyroscope
    fig_gyro = plt.Figure(figsize=(6, 4), dpi=100)
    ax_gyro = fig_gyro.add_subplot(111)
    ax_gyro.set_title("Vận tốc góc (Gyroscope) - Waiting for data...")
    ax_gyro.set_ylabel("Giá trị")
    ax_gyro.set_xlabel("Thời gian (Điểm)")
    line_gx, = ax_gyro.plot([], [], label='Gx')
    line_gy, = ax_gyro.plot([], [], label='Gy')
    line_gz, = ax_gyro.plot([], [], label='Gz')
    ax_gyro.legend()
    ax_gyro.set_ylim(-35000, 35000)
    ax_gyro.set_xlim(0, MAX_PLOT_POINTS)

def update_acc_plot(frame):
    """Update accelerometer plot for the selected node."""
    if selected_node_for_plot and selected_node_for_plot in plot_data_buffers:
        data = plot_data_buffers[selected_node_for_plot]
        x_data = list(range(len(data["ax"])))
        y_ax_data = list(data["ax"])
        y_ay_data = list(data["ay"])
        y_az_data = list(data["az"])

        line_ax.set_data(x_data, y_ax_data)
        line_ay.set_data(x_data, y_ay_data)
        line_az.set_data(x_data, y_az_data)

        all_y_data = y_ax_data + y_ay_data + y_az_data
        if all_y_data:
            min_val = np.min(all_y_data)
            max_val = np.max(all_y_data)
            padding = (max_val - min_val) * 0.1 if (max_val - min_val) != 0 else 1
            ax_acc.set_ylim(min_val - padding, max_val + padding)

        if x_data:
            ax_acc.set_xlim(x_data[0], x_data[-1] + 1)
        else:
            ax_acc.set_xlim(0, MAX_PLOT_POINTS)

        ax_acc.set_title(f"Gia tốc (Accelerometer) - Node: {selected_node_for_plot}")
        return line_ax, line_ay, line_az
    return []

def update_gyro_plot(frame):
    """Update gyroscope plot for the selected node."""
    if selected_node_for_plot and selected_node_for_plot in plot_data_buffers:
        data = plot_data_buffers[selected_node_for_plot]
        x_data = list(range(len(data["gx"])))
        y_gx_data = list(data["gx"])
        y_gy_data = list(data["gy"])
        y_gz_data = list(data["gz"])

        line_gx.set_data(x_data, y_gx_data)
        line_gy.set_data(x_data, y_gy_data)
        line_gz.set_data(x_data, y_gz_data)

        all_y_data = y_gx_data + y_gy_data + y_gz_data
        if all_y_data:
            min_val = np.min(all_y_data)
            max_val = np.max(all_y_data)
            padding = (max_val - min_val) * 0.1 if (max_val - min_val) != 0 else 1
            ax_gyro.set_ylim(min_val - padding, max_val + padding)

        if x_data:
            ax_gyro.set_xlim(x_data[0], x_data[-1] + 1)
        else:
            ax_gyro.set_xlim(0, MAX_PLOT_POINTS)

        ax_gyro.set_title(f"Vận tốc góc (Gyroscope) - Node: {selected_node_for_plot}")
        return line_gx, line_gy, line_gz
    return []

# --- Functions to remember and load directory ---
def save_last_directory(directory_path):
    try:
        with open(CONFIG_FILE, 'w') as f:
            f.write(directory_path)
    except IOError as e:
        print(f"Không thể lưu cấu hình: {e}")

def load_last_directory():
    if os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, 'r') as f:
                return f.read().strip()
        except IOError as e:
            print(f"Không thể tải cấu hình: {e}")
    return None

# --- Data Recording Functions ---
def select_output_directory():
    global output_directory
    folder_selected = filedialog.askdirectory(initialdir=output_directory)
    if folder_selected:
        output_directory = folder_selected
        output_dir_label.config(text=f"Thư mục: {output_directory}")
        log_text.insert(tk.END, f"[INFO] Thư mục lưu dữ liệu được chọn: {output_directory}\n")
        log_text.see(tk.END)
        toggle_recording_buttons_state()
        save_last_directory(output_directory)

def get_output_filename(sensor_id, base_name):
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"{sensor_id}_{base_name}_{timestamp}.csv"
    return os.path.join(output_directory, filename)

def recording_worker():
    while not recording_thread_stop.is_set() or recording_queue:
        try:
            data = recording_queue.popleft()
            write_data_to_file(data['id'], data)
        except IndexError:
            time.sleep(0.005)  # Không có dữ liệu thì nghỉ nhẹ
        except Exception as e:
            log_text.insert(tk.END, f"[ERROR] Thread ghi file lỗi: {e}\n")
            log_text.see(tk.END)


def start_recording_data():
    global is_recording, active_recording_file_handles, recording_thread, recording_thread_stop

    if not running:
        messagebox.showwarning("Cảnh báo", "Vui lòng kết nối thiết bị trước khi ghi dữ liệu.")
        return

    base_file_name = file_name_entry.get().strip()
    if not base_file_name:
        messagebox.showwarning("Cảnh báo", "Vui lòng nhập tên file để lưu dữ liệu.")
        return

    if not output_directory or not os.path.isdir(output_directory):
        try:
            os.makedirs(output_directory, exist_ok=True)
            log_text.insert(tk.END, f"[INFO] Đã tạo thư mục lưu dữ liệu: {output_directory}\n")
        except Exception as e:
            messagebox.showerror("Lỗi", f"Không thể tạo thư mục:\n{e}")
            return

    is_recording = True
    active_recording_file_handles.clear()
    record_button.config(text="Dừng Ghi", command=stop_recording_data, state=tk.NORMAL)
    save_current_data_button.config(state=tk.DISABLED)
    log_text.insert(tk.END, "[INFO] Đã bắt đầu ghi dữ liệu.\n")
    log_text.see(tk.END)
    toggle_recording_buttons_state()

    # Bắt đầu thread ghi
    recording_thread_stop.clear()
    recording_thread = threading.Thread(target=recording_worker, daemon=True)
    recording_thread.start()



def stop_recording_data():
    global is_recording, active_recording_file_handles, recording_thread, recording_thread_stop
    is_recording = False

    # Kết thúc thread ghi an toàn
    recording_thread_stop.set()
    if recording_thread is not None:
        recording_thread.join(timeout=5)

    # Đóng file
    for sensor_id, file_handle in list(active_recording_file_handles.items()):
        try:
            file_handle.close()
            log_text.insert(tk.END, f"[INFO] Đã đóng file ghi dữ liệu cho Node '{sensor_id}'.\n")
        except Exception as e:
            log_text.insert(tk.END, f"[ERROR] Lỗi khi đóng file cho Node '{sensor_id}': {e}\n")
        finally:
            del active_recording_file_handles[sensor_id]

    recording_queue.clear()
    record_button.config(text="Bắt đầu Ghi", command=start_recording_data)
    log_text.insert(tk.END, "[INFO] Đã dừng ghi dữ liệu. Tất cả file đã được đóng.\n")
    toggle_recording_buttons_state()
    toggle_save_button_state()
 # Re-enable Save button after stopping recording

def write_data_to_file(sensor_id, data):
    global is_recording, active_recording_file_handles

    if not is_recording:
        return

    base_file_name = file_name_entry.get().strip()
    if not base_file_name:
        log_text.insert(tk.END, f"[ERROR] Không có tên file để ghi dữ liệu cho Node '{sensor_id}'.\n")
        log_text.see(tk.END)
        return

    # Mở file nếu chưa mở cho node này
    if sensor_id not in active_recording_file_handles:
        output_path= get_output_filename(sensor_id, base_file_name)
        if not output_path:
            log_text.insert(tk.END, f"[ERROR] Đường dẫn file không hợp lệ cho Node '{sensor_id}'.\n")
            log_text.see(tk.END)
            return

        try:
            file_handle = open(output_path, 'a', encoding='utf-8')

            # Ghi header nếu file mới
            if os.path.getsize(output_path) == 0:
                header = "ID,Status,AccX,AccY,AccZ,GyroX,GyroY,GyroZ,Timestamp,Timestamp_us\n"
                file_handle.write(header)

            active_recording_file_handles[sensor_id] = file_handle

            log_text.insert(tk.END, f"[INFO] Đã tạo file ghi dữ liệu cho Node '{sensor_id}': {output_path}\n")
            log_text.see(tk.END)
        except IOError as e:
            log_text.insert(tk.END, f"[ERROR] Không thể mở file để ghi cho Node '{sensor_id}': {e}\n")
            log_text.see(tk.END)
            return

    # Ghi 1 dòng dữ liệu CSV
    file_handle = active_recording_file_handles[sensor_id]
    try:
        csv_line = (
            f"{data['id']},"
            f"{data.get('status', 'Active')},"
            f"{data['ax']:.2f},"
            f"{data['ay']:.2f},"
            f"{data['az']:.2f},"
            f"{data['gx']:.2f},"
            f"{data['gy']:.2f},"
            f"{data['gz']:.2f},"
            f"{data['ts_formatted']},"
            f"{data['ts_us']}\n"
        )
        file_handle.write(csv_line)
        file_handle.flush()
    except Exception as e:
        log_text.insert(tk.END, f"[ERROR] Ghi dữ liệu vào file lỗi cho Node '{sensor_id}': {e}\n")
        log_text.see(tk.END)
        try:
            file_handle.close()
            del active_recording_file_handles[sensor_id]
        except Exception as close_error:
            log_text.insert(tk.END, f"[ERROR] Không thể đóng file lỗi: {close_error}\n")
            log_text.see(tk.END)

def save_current_treeview_data():
    def worker():
        if not connected_nodes_data:
            root.after(0, lambda: messagebox.showinfo("Thông báo", "Không có dữ liệu để lưu."))
            return

        base_file_name = file_name_entry.get().strip()
        if not base_file_name:
            root.after(0, lambda: messagebox.showwarning("Cảnh báo", "Vui lòng nhập tên file để lưu dữ liệu."))
            return

        if not output_directory or not os.path.isdir(output_directory):
            try:
                os.makedirs(output_directory, exist_ok=True)
                root.after(0, lambda: log_text.insert(tk.END, f"[INFO] Đã tạo thư mục: {output_directory}\n"))
            except Exception as e:
                root.after(0, lambda: messagebox.showerror("Lỗi", f"Không thể tạo thư mục:\n{e}"))
                return

        for node_id, data in connected_nodes_data.items():
            save_path = get_output_filename(node_id, base_file_name)
            if not save_path:
                root.after(0, lambda nid=node_id: log_text.insert(tk.END, f"[ERROR] Đường dẫn file không hợp lệ cho Node '{nid}'.\n"))
                continue

            try:
                with open(save_path, 'w', encoding='utf-8') as f:
                    header = "ID,Status,AccX,AccY,AccZ,GyroX,GyroY,GyroZ,Timestamp,Timestamp_us\n"
                    f.write(header)
                    csv_line = (
                        f"{data['id']},{data.get('status', 'Active')},"
                        f"{data['ax']:.2f},{data['ay']:.2f},{data['az']:.2f},"
                        f"{data['gx']:.2f},{data['gy']:.2f},{data['gz']:.2f},"
                        f"{data['ts_formatted']},{data['ts_us']}\n"
                    )
                    f.write(csv_line)
                root.after(0, lambda nid=node_id, path=save_path:
                           log_text.insert(tk.END, f"[INFO] Dữ liệu Node '{nid}' đã được lưu vào: {path}\n"))
            except Exception as e:
                root.after(0, lambda nid=node_id:
                           messagebox.showerror("Lỗi lưu file", f"Không thể lưu dữ liệu cho Node '{nid}':\n{e}"))
                continue

        root.after(0, lambda: [log_text.see(tk.END),
                               messagebox.showinfo("Thành công", f"Dữ liệu hiện tại của các Sensor Node đã được lưu vào:\n{output_directory}")])

    threading.Thread(target=worker, daemon=True).start()
# --- Toggle Button States ---
def toggle_recording_buttons_state():
    if not all(hasattr(w, 'config') for w in [file_name_entry, select_dir_button, record_button, save_current_data_button]):
        return

    file_name_ready = bool(file_name_entry.get().strip())
    dir_selected = os.path.exists(output_directory) and os.path.isdir(output_directory)

    if running:
        if not is_recording:
            record_button.config(state=tk.NORMAL if file_name_ready and dir_selected else tk.DISABLED)
            record_button.config(text="Bắt đầu Ghi", command=start_recording_data)
            save_current_data_button.config(state=tk.NORMAL if connected_nodes_data and file_name_ready and dir_selected else tk.DISABLED)
        else:
            record_button.config(state=tk.NORMAL)
            record_button.config(text="Dừng Ghi", command=stop_recording_data)
            save_current_data_button.config(state=tk.DISABLED)

        file_name_entry.config(state=tk.NORMAL if not is_recording else tk.DISABLED)
        select_dir_button.config(state=tk.NORMAL if not is_recording else tk.DISABLED)
    else:
        record_button.config(state=tk.DISABLED)
        save_current_data_button.config(state=tk.DISABLED)
        file_name_entry.config(state=tk.NORMAL)
        select_dir_button.config(state=tk.NORMAL)

def toggle_save_button_state():
    if not hasattr(save_current_data_button, 'config'):
        return

    file_name_ready = bool(file_name_entry.get().strip())
    dir_selected = os.path.exists(output_directory) and os.path.isdir(output_directory)
    save_current_data_button.config(state=tk.NORMAL if connected_nodes_data and not is_recording and file_name_ready and dir_selected else tk.DISABLED)


# --- Node Selection for Plotting ---
def update_node_selection_combobox():
    global selected_node_for_plot
    node_ids = sorted(list(connected_nodes_data.keys()))
    node_select_combobox['values'] = node_ids

    if node_ids:
        # Nếu selected_node_for_plot không còn tồn tại -> chọn lại
        if selected_node_for_plot not in node_ids:
            selected_node_for_plot = node_ids[0]
            node_select_combobox.set(selected_node_for_plot)
        node_select_combobox.config(state="readonly")
    else:
        selected_node_for_plot = None
        node_select_combobox.set("Chọn Node")
        node_select_combobox.config(state="disabled")


def on_node_selected_for_plot(event):
    global ani_acc, ani_gyro, selected_node_for_plot
    node = node_select_combobox.get()

    if node not in connected_nodes_data:
        return  # Node không tồn tại, bỏ qua

    selected_node_for_plot = node

    # Dừng animation cũ
    if ani_acc: ani_acc.event_source.stop()
    if ani_gyro: ani_gyro.event_source.stop()

    # Nếu đang chạy, khởi động lại vẽ
    if running:
        ani_acc = FuncAnimation(fig_acc, update_acc_plot, interval=100, blit=False)
        ani_gyro = FuncAnimation(fig_gyro, update_gyro_plot, interval=100, blit=False)

    fig_acc_canvas.draw_idle()
    fig_gyro_canvas.draw_idle()



# --- GUI Setup ---
root = tk.Tk()
root.title("ESP32 Sensor Data Reader with Plotting & Recording")
root.state('zoomed') # Open in full screen (Windows)

style = ttk.Style()
style.configure("Red.TLabel", foreground="red")
style.configure("Green.TLabel", foreground="green")
style.configure("Orange.TLabel", foreground="orange")

# Configure Grid for root window
root.grid_rowconfigure(0, weight=0) # Toolbar Frame (no expansion)
root.grid_rowconfigure(1, weight=0) # Log Frame (no expansion)
root.grid_rowconfigure(2, weight=1) # Main Content Frame (expands and fills remaining space)
root.grid_columnconfigure(0, weight=1) # Single column occupies full width

# --- Toolbar Frame (Row 0) ---
toolbar_frame = ttk.LabelFrame(root, text="Điều khiển & Ghi dữ liệu")
toolbar_frame.grid(row=0, column=0, sticky="ew", padx=10, pady=5)

# Configure grid for toolbar_frame (more columns for new elements)
toolbar_frame.grid_columnconfigure(0, weight=0) # "Cổng COM:" label
toolbar_frame.grid_columnconfigure(1, weight=0) # com_port_combobox
toolbar_frame.grid_columnconfigure(2, weight=0) # "Select Node:" label
toolbar_frame.grid_columnconfigure(3, weight=0) # node_select_combobox
toolbar_frame.grid_columnconfigure(4, weight=0) # "Tên File:" label
toolbar_frame.grid_columnconfigure(5, weight=1) # file_name_entry (expands)
toolbar_frame.grid_columnconfigure(6, weight=0) # Select Dir button
toolbar_frame.grid_columnconfigure(7, weight=0) # Dir Label
toolbar_frame.grid_columnconfigure(8, weight=0) # Connect button
toolbar_frame.grid_columnconfigure(9, weight=0) # Disconnect button
toolbar_frame.grid_columnconfigure(10, weight=0) # Start/Stop Record button
toolbar_frame.grid_columnconfigure(11, weight=0) # Save Current Data button
toolbar_frame.grid_columnconfigure(12, weight=0) # Exit button
toolbar_frame.grid_columnconfigure(13, weight=0) # Status Label

# Widgets in Toolbar Frame
com_port_label = ttk.Label(toolbar_frame, text="Cổng COM:")
com_port_label.grid(row=0, column=0, padx=5, pady=5, sticky="w")

com_port_combobox = ttk.Combobox(toolbar_frame, state="readonly", width=15)
com_port_combobox.grid(row=0, column=1, padx=5, pady=5, sticky="ew")

# Node Selection Combobox
node_select_label = ttk.Label(toolbar_frame, text="Chọn Node:")
node_select_label.grid(row=0, column=2, padx=(10, 5), pady=5, sticky="w")

node_select_combobox = ttk.Combobox(toolbar_frame, state="disabled", width=15)
node_select_combobox.grid(row=0, column=3, padx=5, pady=5, sticky="ew")
node_select_combobox.set("Không có Node")
node_select_combobox.bind("<<ComboboxSelected>>", on_node_selected_for_plot)


ttk.Label(toolbar_frame, text="Tên File:").grid(row=0, column=4, padx=(10, 5), pady=5, sticky="w")
file_name_entry = ttk.Entry(toolbar_frame, width=30)
file_name_entry.grid(row=0, column=5, padx=5, pady=5, sticky="ew")
file_name_entry.insert(0, "sensor_data") # Default file name

select_dir_button = ttk.Button(toolbar_frame, text="Mở Thư mục", command=select_output_directory)
select_dir_button.grid(row=0, column=6, padx=5, pady=5)

output_dir_label = ttk.Label(toolbar_frame, text=f"Thư mục: {output_directory}", wraplength=200)
output_dir_label.grid(row=0, column=7, padx=5, pady=5, sticky="w")

# Connection Control Buttons
connect_button = ttk.Button(toolbar_frame, text="Kết nối", command=start_serial_read_thread)
connect_button.grid(row=0, column=8, padx=(10, 2), pady=5)

disconnect_button = ttk.Button(toolbar_frame, text="Ngắt kết nối", command=stop_serial_read, state=tk.DISABLED)
disconnect_button.grid(row=0, column=9, padx=(2, 10), pady=5)

# Data Recording Control Buttons
record_button = ttk.Button(toolbar_frame, text="Bắt đầu Ghi", command=start_recording_data, state=tk.DISABLED)
record_button.grid(row=0, column=10, padx=(10, 2), pady=5)

save_current_data_button = ttk.Button(toolbar_frame, text="Lưu dữ liệu hiện tại", command=save_current_treeview_data, state=tk.DISABLED)
save_current_data_button.grid(row=0, column=11, padx=(2, 10), pady=5)

exit_button = ttk.Button(toolbar_frame, text="Thoát", command=on_closing)
exit_button.grid(row=0, column=12, padx=(10, 5), pady=5)

status_label = ttk.Label(toolbar_frame, text="Không kết nối", style="Red.TLabel")
status_label.grid(row=0, column=13, padx=10, pady=5, sticky="e")


# --- Log Frame (Row 1) ---
log_frame = ttk.LabelFrame(root, text="Serial Log (Dữ liệu RAW và thông báo hệ thống)")
log_frame.grid(row=1, column=0, sticky="ew", padx=10, pady=5)

log_text = scrolledtext.ScrolledText(log_frame, wrap=tk.WORD, height=8, font=("Consolas", 9))
log_text.pack(padx=5, pady=5, fill=tk.BOTH, expand=True)
log_text.insert(tk.END, "Chào mừng! Chọn cổng COM và nhấn 'Kết nối' để bắt đầu.\n")
log_text.see(tk.END)


# --- Main Content Frame (Row 2, Column 0) ---
main_content_frame = ttk.Frame(root)
main_content_frame.grid(row=2, column=0, sticky="nsew", padx=10, pady=5)

# Configure Grid for main_content_frame
main_content_frame.grid_columnconfigure(0, weight=1) # Left column (Treeview)
main_content_frame.grid_columnconfigure(1, weight=2) # Right column (plots) will be twice as wide
main_content_frame.grid_rowconfigure(0, weight=1) # Single row, expands

# Left Column (Treeview)
left_column_frame = ttk.Frame(main_content_frame)
left_column_frame.grid(row=0, column=0, sticky="nsew", padx=(0, 5))
left_column_frame.grid_rowconfigure(0, weight=1)
left_column_frame.grid_columnconfigure(0, weight=1)

# Data Treeview Frame
data_frame = ttk.LabelFrame(left_column_frame, text="Dữ liệu Sensor Node")
data_frame.grid(row=0, column=0, sticky="nsew", pady=5)

columns = ("ID", "Trạng thái", "AccX", "AccY", "AccZ", "GyroX", "GyroY", "GyroZ", "Timestamp", "Timestamp_us")
data_tree = ttk.Treeview(data_frame, columns=columns, show="headings")

for col in columns:
    data_tree.heading(col, text=col)
    data_tree.column(col, width=80, anchor=tk.CENTER)

data_tree.column("ID", width=70)
data_tree.column("Trạng thái", width=90)
data_tree.column("AccX", width=55)
data_tree.column("AccY", width=55)
data_tree.column("AccZ", width=55)
data_tree.column("GyroX", width=55)
data_tree.column("GyroY", width=55)
data_tree.column("GyroZ", width=55)
data_tree.column("Timestamp", width=110)
data_tree.column("Timestamp_us", width=80)

data_tree.pack(padx=5, pady=5, fill=tk.BOTH, expand=True)

# Right Column (Plots)
right_column_frame = ttk.Frame(main_content_frame)
right_column_frame.grid(row=0, column=1, sticky="nsew", padx=(5, 0))

# Configure Grid for right_column_frame to split 2 plots
right_column_frame.grid_rowconfigure(0, weight=1)
right_column_frame.grid_rowconfigure(1, weight=1)
right_column_frame.grid_columnconfigure(0, weight=1)

# Setup Matplotlib figures
setup_plots()

# Accelerometer Plot Frame
acc_plot_frame = ttk.LabelFrame(right_column_frame, text="Đồ thị Gia tốc (Accelerometer)")
acc_plot_frame.grid(row=0, column=0, sticky="nsew", pady=5)
fig_acc_canvas = FigureCanvasTkAgg(fig_acc, master=acc_plot_frame)
fig_acc_canvas_widget = fig_acc_canvas.get_tk_widget()
fig_acc_canvas_widget.pack(side=tk.TOP, fill=tk.BOTH, expand=True)

# Gyroscope Plot Frame
gyro_plot_frame = ttk.LabelFrame(right_column_frame, text="Đồ thị Vận tốc góc (Gyroscope)")
gyro_plot_frame.grid(row=1, column=0, sticky="nsew", pady=5)
fig_gyro_canvas = FigureCanvasTkAgg(fig_gyro, master=gyro_plot_frame)
fig_gyro_canvas_widget = fig_gyro_canvas.get_tk_widget()
fig_gyro_canvas_widget.pack(side=tk.TOP, fill=tk.BOTH, expand=True)


# --- Initialize GUI Application ---
# Load last selected directory, or use current script directory
last_dir = load_last_directory()
if last_dir and os.path.isdir(last_dir):
    output_directory = last_dir

# Create default directory if it doesn't exist
if not os.path.exists(output_directory):
    os.makedirs(output_directory)

# Update initial output directory label
output_dir_label.config(text=f"Thư mục: {output_directory}")

# Update COM ports after combobox is created
update_com_ports()

# Update initial state of recording/save buttons
toggle_recording_buttons_state()
toggle_save_button_state()

# Update node selection combobox initially (it will be empty/disabled)
update_node_selection_combobox()

root.protocol("WM_DELETE_WINDOW", on_closing)
root.mainloop()