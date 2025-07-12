import json
import argparse
from pathlib import Path
from jinja2 import Environment, FileSystemLoader
import datetime
GLSL_TYPE_TO_CPP = {
    "float": "GLfloat",
    "vec2": "glm::vec2",
    "vec3": "glm::vec3",
    "vec4": "glm::vec4",
    "mat4": "glm::mat4",
    "mat3": "glm::mat3",
    "int": "GLint",
    "bool": "GLboolean",
    "uint": "GLuint",
}

def glsl_type_to_cpp(glsl_type):
    return GLSL_TYPE_TO_CPP.get(glsl_type, glsl_type)

def process_shaders(json_file_paths):
    merged_data = {
        "inputs": [],
        "samplers": [],
        "ubos": [],
        "storage_buffers": [],
    }
    input_locations = set()
    sampler_bindings = set()
    ubo_bindings = set()
    ssbo_bindings = set()
    
    for json_path in json_file_paths:
        with open(json_path, 'r') as file:
            data = json.load(file)
            # merge inputs vertex attributes
            for item in data.get("inputs", []):
                if data["shader_stage"] == "vertex" and item["location"] not in input_locations:
                    merged_data["inputs"].append(item)
                    input_locations.add(item["location"])
            # merge samplers
            for item in data.get("samplers", []):
                if item["binding"] not in sampler_bindings:
                    merged_data["samplers"].append(item)
                    sampler_bindings.add(item["binding"])
            # merge ubos
            for ubo in data.get("uniforms", []):
                if ubo["binding"] not in ubo_bindings:
                    
                    processed_members = []
                    current_offset = 0
                    padding_index = 0

                    for member in sorted(ubo["members"], key=lambda m: m["offset_bytes"]):
                        # 检查是否需要在当前成员之前添加 padding
                        if member["offset_bytes"] > current_offset:
                            padding_size = member["offset_bytes"] - current_offset
                            processed_members.append({
                                "is_padding": True,
                                "name": f"padding_{padding_index}",
                                "size_bytes": padding_size
                            })
                            padding_index += 1
                        
                        # 添加原始成员
                        # 使用 ** 语法来复制原始成员字典，并添加一个标志
                        processed_members.append({**member, "is_padding": False})

                        # 更新当前偏移量
                        current_offset = member["offset_bytes"] + member["size_bytes"]
                    
                    # 检查是否需要在结构体末尾添加 padding
                    if ubo["size_bytes"] > current_offset:
                        padding_size = ubo["size_bytes"] - current_offset
                        processed_members.append({
                            "is_padding": True,
                            "name": f"padding_{padding_index}",
                            "size_bytes": padding_size
                        })

                    # 用处理过的新成员列表替换旧的
                    ubo["members"] = processed_members
                    merged_data["ubos"].append(ubo)
                    ubo_bindings.add(ubo["binding"])
            for item in data.get("storage_buffers", []):
                if item["binding"] not in ssbo_bindings:
            
                    # 1. 分离固定成员和运行时数组成员
                    runtime_member = None
                    fixed_members_raw = []
                    # 在你的 JSON 中，成员列表的键是 "struct_members"，我们统一用 "members"
                    # 以便模板中可以重用逻辑
                    if "struct_members" in item:
                        item["members"] = item.pop("struct_members")

                    for member in item.get("members", []):
                        if member.get("is_runtime_array", False):
                            runtime_member = member
                        else:
                            fixed_members_raw.append(member)
            
                    # 2. 为固定成员部分计算 padding (和之前 UBO 的逻辑一样)
                    processed_fixed_members = []
                    current_offset = 0
                    padding_index = 0

                    # block_size_bytes 是固定部分的总大小
                    fixed_part_total_size = item.get("block_size_bytes", 0)

                    for member in sorted(fixed_members_raw, key=lambda m: m["offset"]):
                        if member["offset"] > current_offset:
                            padding_size = member["offset"] - current_offset
                            processed_fixed_members.append({
                            "is_padding": True,
                            "name": f"padding_{padding_index}",
                            "size_bytes": padding_size
                        })
                            padding_index += 1
                
                        # 计算成员大小（需要考虑固定大小数组）
                        # 你的反射 JSON 中 "size_bytes" 对于固定数组成员已经是整个数组的大小了，很方便
                        member_size = member["size_bytes"]
                
                        # 添加原始成员
                        processed_fixed_members.append({**member, "is_padding": False})
                        current_offset = member["offset"] + member_size

                    # 检查固定部分末尾是否需要 padding
                    if fixed_part_total_size > current_offset:
                        padding_size = fixed_part_total_size - current_offset
                        processed_fixed_members.append({
                    "is_padding": True,
                    "name": f"padding_{padding_index}",
                    "size_bytes": padding_size
                })

                    # 3. 将处理好的数据放回 item 中，供模板使用
                    item["fixed_members"] = processed_fixed_members
                    item["runtime_member"] = runtime_member # 可能是 None
            
                    merged_data["storage_buffers"].append(item)
                    ssbo_bindings.add(item["binding"])


    return merged_data

def cli_parse():
    parser = argparse.ArgumentParser(description="Generate C++ shader bindings from SPIR-V reflection JSON.")
    parser.add_argument("--jsons", nargs='+', required=True, help="List of input JSON reflection files (e.g., vert.json frag.json).")
    parser.add_argument("--name", required=True, help="The base name for the generated C++ class (e.g., PhongShader).")
    parser.add_argument("--outdir", type=Path, required=True, help="Output directory for the generated .hpp and .cpp files.")
    parser.add_argument("--templates", type=Path, required=True, help="Directory containing Jinja2 templates.")

    args = parser.parse_args()
    return args

def main():
    args = cli_parse()

    reflection_data = process_shaders(args.jsons)


    templates = Path(args.templates).rglob('*.jinja')
    env = Environment(
            loader=FileSystemLoader(args.templates),
            trim_blocks=True,
            lstrip_blocks=True,

    )
    env.globals["datetime"] = datetime
    env.globals["glsl_type_to_cpp"] = glsl_type_to_cpp
    for templatePath in list(templates):

        template = env.get_template(templatePath.name)

        output = template.render(reflection_data)

        # 获取模板文件名并替换扩展名为 .cpp 或 .hpp
        template_name = templatePath.name
        output_file_name = template_name.replace('.jinja','')

        # 确保输出目录存在
        output_dir = Path(args.outdir)
        output_dir.mkdir(parents=True, exist_ok=True)

        # 构建输出路径
        output_path = output_dir / output_file_name

        # 写入文件
        with open(output_path, 'w') as f:
            f.write(output)


if __name__ == "__main__":
    main()