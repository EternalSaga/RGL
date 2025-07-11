#include "SSBO.hpp"


#include <cassert>
#include <cstddef>
#include <stdexcept>

#include <array>
#include <memory>
#include <mutex>

#include "UBO.hpp"




namespace RGL {
namespace glcore {




void SSBO::prepareOffsetsAndDataType() {
    blockIndex = glCall(glGetProgramResourceIndex, shader, GL_SHADER_STORAGE_BLOCK, ssboName.c_str());
    if (blockIndex == GL_INVALID_INDEX) {
	logger->critical("SSBO: Invalid block index for SSBO name {} in shader {}", ssboName, shader);
	throw std::invalid_argument("Invalid block index for SSBO name " + ssboName + " in shader " + std::to_string(shader));
    }
    GLenum properties[] = {GL_BUFFER_DATA_SIZE, GL_NUM_ACTIVE_VARIABLES};
    GLint results[2];
    glCall(glGetProgramResourceiv, shader, GL_SHADER_STORAGE_BLOCK, blockIndex, 2, properties, 2, nullptr, results);
    blockSize = results[0];
    storagesCount = results[1];
    if (storagesCount != 0) {
	logger->info("SSBO: Block size for SSBO name {} in shader {} is {}", ssboName, shader, blockSize);
    } else {
	logger->warn("SSBO: No active variables found for SSBO name {} in shader {}", ssboName, shader);
	data.resize(blockSize);
	std::fill(data.begin(), data.end(), std::byte(0x00));
	return;
    }
    data.resize(blockSize);
    std::fill(data.begin(), data.end(), std::byte(0x00));

    std::vector<GLuint> variableIndices(storagesCount);

    GLenum propertiesIndices = GL_ACTIVE_VARIABLES;
    glCall(glGetProgramResourceiv, shader, GL_SHADER_STORAGE_BLOCK, blockIndex, 1, &propertiesIndices, storagesCount, nullptr, reinterpret_cast<GLint*>(variableIndices.data()));

    storageOffsets.resize(storagesCount);
    storageTypes.resize(storagesCount);
    storageArrayStrides.resize(storagesCount);
    storageSizes.resize(storagesCount);
    storageNames.resize(storagesCount);
    GLenum varProperties[] = {
	GL_OFFSET,
	GL_TYPE,
	GL_ARRAY_STRIDE,
	GL_NAME_LENGTH,
	GL_ARRAY_SIZE};

    const GLsizei numVarProps = sizeof(varProperties) / sizeof(varProperties[0]);

    for (size_t i = 0; i < storagesCount; ++i) {
	GLint propResults[numVarProps];
	glCall(glGetProgramResourceiv, shader, GL_BUFFER_VARIABLE, variableIndices[i], numVarProps, varProperties, numVarProps, nullptr, reinterpret_cast<GLint*>(propResults));
	storageOffsets[i] = propResults[0];
	storageTypes[i] = static_cast<GLenum>(propResults[1]);
	storageArrayStrides[i] = propResults[2];
	GLint nameLength = propResults[3];
	storageNames[i].resize(nameLength);
	glCall(glGetProgramResourceName, shader, GL_BUFFER_VARIABLE, variableIndices[i], nameLength, nullptr, storageNames[i].data());
    }

    for (size_t i = 0; i < storagesCount; ++i) {
	logger->info("Storage {} offset: {}, type: {}, array stride: {}, name: {}", i, storageOffsets[i], type2string(storageTypes[i]), storageArrayStrides[i], storageNames[i]);
    }
}

SSBO::SSBO(GLuint shader, std::string ssboName,GLint bindingPoint) {
    this->ssboName = ssboName;
    this->shader = shader;

    prepareOffsetsAndDataType();

    glCall(glCreateBuffers, 1, &ssbo);

    glCall(glNamedBufferData, ssbo, blockSize, nullptr, GL_DYNAMIC_DRAW);

    this->bindingPoint = bindingPoint;

    glCall(glBindBufferBase, GL_SHADER_STORAGE_BUFFER, this->bindingPoint, ssbo);
}


void SSBO::updateBufferData(std::vector<std::byte> cpuData){
    assert(cpuData.size() <= blockSize);
    glCall(glNamedBufferSubData, this->ssbo, 0, cpuData.size(), cpuData.data());
}



}  // namespace glcore
}  // namespace RGL