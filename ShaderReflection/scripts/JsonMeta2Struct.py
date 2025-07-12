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
            for item in data.get("uniforms", []):
                if item["binding"] not in ubo_bindings:
                    merged_data["ubos"].append(item)
                    ubo_bindings.add(item["binding"])
            # merge ssbos
            for item in data.get("storage_buffers", []):
                if item["binding"] not in ssbo_bindings:
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