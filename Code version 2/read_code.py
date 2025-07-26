import ast

def get_function_names_from_file(file_path):
    with open(file_path, 'r', encoding='utf-8') as f:
        file_content = f.read()
    
    tree = ast.parse(file_content)
    func_names = [node.name for node in ast.walk(tree) if isinstance(node, ast.FunctionDef)]
    return func_names

# Ví dụ sử dụng
file_path = '\\EspServer.py'  # Đổi thành đường dẫn file thật
function_names = get_function_names_from_file(file_path)
print("Các hàm được định nghĩa trong file:")
for name in function_names:
    print(name)