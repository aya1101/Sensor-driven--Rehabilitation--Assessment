import serial
import json
import time
import datetime
import logging

# --- Cấu hình Logging ---
# Để thấy các gói dữ liệu JSON được in trực tiếp ra console,
# hãy đảm bảo dòng dưới đây KHÔNG bị comment.
logging.basicConfig(level=logging.INFO, format='[%(levelname)s] %(message)s')
# Nếu bạn muốn xem thêm các thông báo debug chi tiết khác, thay đổi level thành DEBUG:
# logging.basicConfig(level=logging.DEBUG, format='[%(levelname)s] %(message)s')


# --- Cấu hình Serial Port ---
# <<< THAY 'COMx' BẰNG CỔNG COM CỦA ESP32 CỦA BẠN (ví dụ: 'COM3', '/dev/ttyUSB0')
SERIAL_PORT = 'COM8'
BAUD_RATE = 115200

# --- Cấu hình File Log ---
log_file_name = f"./sensor_data_log_{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
log_file = None

# --- Khởi tạo Serial Port ---
logging.info(f"Đang cố gắng kết nối tới cổng Serial: {SERIAL_PORT} với tốc độ {BAUD_RATE} baud.")
try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    logging.info(f"Đã kết nối thành công cổng Serial: {SERIAL_PORT}.")
except serial.SerialException as e:
    logging.error(f"Lỗi mở cổng Serial {SERIAL_PORT}: {e}")
    logging.info("Vui lòng kiểm tra cổng có đúng không và không bị chương trình khác sử dụng (ví dụ: Arduino IDE).")
    exit()

# --- Mở File Log ---
logging.info(f"Đang cố gắng mở file log: {log_file_name}")
try:
    log_file = open(log_file_name, 'w', buffering=1, encoding='utf-8')
    log_file.write("NodeID,ax,ay,az,gx,gy,gz,Timestamp_Local, Timestamp_Node\n")
    logging.info(f"File log đã được mở thành công. Dữ liệu sẽ được ghi vào: {log_file_name}")
except IOError as e:
    logging.error(f"Không thể mở hoặc tạo file log {log_file_name}: {e}")
    logging.info("Đảm bảo bạn có quyền ghi vào thư mục hiện tại.")
    ser.close()
    exit()

logging.info("Chương trình đang chạy. Nhấn Ctrl+C để dừng.")
logging.info("--- Dữ liệu JSON nhận được sẽ hiển thị bên dưới ---")

# --- Vòng lặp đọc dữ liệu từ Serial ---
try:
    while True:
        if ser.in_waiting > 0:
            try:
                line = ser.readline().decode('utf-8').strip()
                
                # Kiểm tra xem dòng có phải là dữ liệu JSON UDP từ ESP32 Server không
                if "Received UDP from" in line and "->" in line:
                    json_start_index = line.find("{")
                    if json_start_index != -1:
                        json_str = line[json_start_index:]
                        try:
                            data = json.loads(json_str)
                            
                            # --- IN DỮ LIỆU JSON ĐỂ DEBUG TRỰC TIẾP ---
                            print(f"[DATA] {json.dumps(data)}") # In dữ liệu JSON trực tiếp ra console
                            # --- HẾT PHẦN IN DEBUG ---

                            # Lấy các trường dữ liệu
                            node_id = data.get('id', 'N/A')
                            ax = data.get('ax', 0)
                            ay = data.get('ay', 0)
                            az = data.get('az', 0)
                            gx = data.get('gx', 0)
                            gy = data.get('gy', 0)
                            gz = data.get('gz', 0)
                            ts_node = data.get('ts', 'N/A')

                            # Ghi dữ liệu vào file log                            
                            log_entry = (                                
                                f"{node_id},{ax},{ay},{az},{gx},{gy},{gz},{ts_node}\n"
                            )
                            log_file.write(log_entry)
                            logging.info(f"Đã ghi dữ liệu từ Node ID: {node_id} vào file log.")

                        except json.JSONDecodeError as e:
                            logging.warning(f"Không thể phân tích JSON từ dòng: '{json_str}'. Lỗi: {e}. Dòng gốc: '{line}'")
                        except KeyError as e:
                            logging.warning(f"Thiếu trường dữ liệu trong JSON: {e}. JSON: '{json_str}'")
                    # else:
                        # logging.debug(f"Dòng không chứa '{'{'}'. Bỏ qua: '{line}'") # Giữ debug này nếu cần
                elif line.startswith("Handshake request from Node ID:"):
                    logging.info(f"Đã phát hiện yêu cầu Handshake từ ESP8266: '{line}'")
                # else:
                    # logging.debug(f"Dòng Serial không phải dữ liệu JSON hoặc Handshake (bỏ qua): '{line}'") # Giữ debug này nếu cần

            except UnicodeDecodeError as e:
                logging.error(f"Lỗi giải mã ký tự từ Serial: {e}. Dòng bị lỗi: '{line}'")
            except Exception as e:
                logging.error(f"Lỗi không xác định khi xử lý dòng Serial: {e}. Dòng: '{line}'")

        time.sleep(0.01)

except KeyboardInterrupt:
    logging.info("Đã nhận lệnh dừng chương trình (Ctrl+C). Đang đóng cổng Serial và file log.")
except Exception as e:
    logging.error(f"Đã xảy ra lỗi bất ngờ và chương trình phải dừng: {e}")

finally:
    logging.info("Đang thực hiện các bước dọn dẹp cuối cùng...")
    if ser.is_open:
        ser.close()
        logging.info("Đã đóng cổng Serial.")
    if log_file:
        log_file.close()
        logging.info("Đã đóng file log.")