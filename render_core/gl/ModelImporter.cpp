#include "ModelImporter.hpp"
#include <assimp/color4.h>
#include <assimp/scene.h>
#include <spdlog/common.h>
#include <windef.h>
#include "EnTTRelationship.hpp"
#include "GLCheckError.hpp"
#include "GLTextures.hpp"
#include "Material.hpp"
#include "Mesh.hpp"
#include "ShaderManager.hpp"
#include "rllogger.hpp"
#include <cstddef>
#include <entt/entity/entity.hpp>
#include <filesystem>
#include <glm/fwd.hpp>
#include <memory>
#include <queue>
#include <stack>
#include <stdexcept>
#include <utility>
#include <vector>
#include "RenderQueue.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>
namespace RGL {
namespace io {

std::vector<GLfloat> aiVertexToFloats(aiMesh* mesh, size_t aiVertexIdx) {
    const auto& vertex = mesh->mVertices[aiVertexIdx];
    glm::vec3 pos{vertex.x, vertex.y, vertex.z};

    const auto& ainormal = mesh->mNormals[aiVertexIdx];
    if (!mesh->HasNormals()) {
	auto logger = RLLogger::getInstance();
	logger->error("This mesh has no normals");
	throw std::invalid_argument("This mesh has no normals");
    }
    glm::vec3 normal{mesh->mNormals[aiVertexIdx].x, mesh->mNormals[aiVertexIdx].y, mesh->mNormals[aiVertexIdx].z};

    glm::vec2 texCoord{0.0f, 0.0f};
    if (mesh->mTextureCoords[0]) {  // 如果模型的第一个纹理坐标不为空，把它当成纹理坐标，有的时候，模型有多套纹理坐标，也可能是空的
	texCoord.x = mesh->mTextureCoords[0][aiVertexIdx].x;
	texCoord.y = mesh->mTextureCoords[0][aiVertexIdx].y;
    }
    return std::vector<GLfloat>{pos.x, pos.y, pos.z, texCoord.x, texCoord.y, normal.x, normal.y, normal.z};
}

std::unique_ptr<Mesh> ModelImporter::processMesh(aiMesh* importedMesh) {
    const auto& numofVertices = importedMesh->mNumVertices;

    auto meshObj = std::make_unique<Mesh>(
	FloatDescs{FloatDesc{"inPos", 3}, FloatDesc{"inUV", 2}, FloatDesc{"inNormal", 3}}, numofVertices);
    // 处理顶点
    for (unsigned int j = 0; j < numofVertices; j++) {
	const auto vertex = aiVertexToFloats(importedMesh, j);
	meshObj->pushVertex(vertex);
    }
    // 处理索引
    for (unsigned int j = 0; j < importedMesh->mNumFaces; j++) {
	auto face = importedMesh->mFaces[j];
	assert(face.mNumIndices == 3);
	for (size_t k = 0; k < face.mNumIndices; k++) {
	    meshObj->pushIndex(face.mIndices[k]);
	}
    }

    // 处理材质
    if (importedMesh->mMaterialIndex >= 0) {
	auto material = importedMesh->mMaterialIndex;

	auto [materialData, pbrComponent] = processMaterial(material);
	meshObj->setMaterial(materialData);
	meshObj->setPBRComponent(pbrComponent);
    }

    return std::move(meshObj);
}

void ModelImporter::processNodeBFS(ShaderRef shader) {
    std::queue<aiNode*> nodeQueue;

    auto rootNode = scene->mRootNode;

    // 先处理下rootNode
    Relationship rootRel;
    rootRel.parent = entt::null;
    rootRel.children = std::vector<entt::entity>{};

    entt::entity rootEntity = singleReg->create();
    nodeMap[rootNode] = rootEntity;
    nodeQueue.push(rootNode);

    while (!nodeQueue.empty()) {
	auto currentNode = nodeQueue.front();
	nodeQueue.pop();

	auto currentEntity = nodeMap[currentNode];
	Relationship currentRel;

	if (currentNode->mParent != nullptr) [[likely]] {
	    currentRel.parent = nodeMap[currentNode->mParent];
	} else [[unlikely]] {
	    currentRel.parent = entt::null;
	}

	// 处理当前节点的子节点，所有的子节点都创建一个entity，并设置成relationship的child
	for (size_t i = 0; i < currentNode->mNumChildren; i++) {
	    auto childNode = currentNode->mChildren[i];
	    entt::entity childEntity = singleReg->create();
	    nodeMap[childNode] = childEntity;
	    nodeQueue.push(childNode);
	    currentRel.children.push_back(childEntity);
	}

	singleReg->emplace<Relationship>(currentEntity, currentRel);
	singleReg->emplace<ShaderRef>(currentEntity, shader);

	singleReg->emplace<DiscreteUniforms>(currentEntity);

	// 处理节点自身mesh
	for (size_t i = 0; i < currentNode->mNumMeshes; i++) {
	    auto meshIndex = currentNode->mMeshes[i];
	    auto mesh = scene->mMeshes[meshIndex];
	    auto meshObj = processMesh(mesh);

	    // 检查mesh的material透明度
	    if (meshObj->getMaterial()->getTransparent()) {
		singleReg->emplace<RenderTags::Transparent>(currentEntity);
	    } else {
		singleReg->emplace<RenderTags::Opaque>(currentEntity);
	    }
	    singleReg->emplace<RenderTags::Renderable>(currentEntity, true);
	    auto vertArrayComp = VAOCreater::createMeshVAO(*meshObj, *shader);
	    auto [vertCount, idxOffset] = meshObj->getIdicesCountAndOffset();
	    singleReg->emplace<VertArrayComponent>(currentEntity, std::move(vertArrayComp), vertCount, idxOffset);

	    const auto sampler = SamplerCreater::createSamplers(*meshObj, *shader);

	    singleReg->emplace<SamplerCreater::Samplers>(currentEntity, sampler);

	    const auto pbrComponent = meshObj->getPBRComponent();
	    if (!pbrComponent.isEmpty) {
		singleReg->emplace<PBRComponent>(currentEntity, pbrComponent);
	    }
	}
    }

    logger->debug("Count of nodes processed: {}", nodeMap.size());
    logger->debug("Count of meshes in root node: {}", rootNode->mNumMeshes);
    logger->debug("Model path: {}", this->modelRootPath.string());

    for (auto& [node, entity] : nodeMap) {
	singleReg->emplace<Transform>(entity, decomposeTransform(node->mTransformation));
	logger->trace("Model node local transform for entity {}, mat4 \na1:{},a2:{},a3:{},a4:{}\nb1:{},b2:{},b3:{}b4:{}\nc1:{},c2:{},c3:{},c4:{}\nd1:{},d2:{},d3:{},d4:{}",
	    (entt::to_integral(entity)),
	    node->mTransformation.a1,
	    node->mTransformation.a2,
	    node->mTransformation.a3,
	    node->mTransformation.a4,
	    node->mTransformation.b1,
	    node->mTransformation.b2,
	    node->mTransformation.b3,
	    node->mTransformation.b4,
	    node->mTransformation.c1,
	    node->mTransformation.c2,
	    node->mTransformation.c3,
	    node->mTransformation.c4,
	    node->mTransformation.d1,
	    node->mTransformation.d2,
	    node->mTransformation.d3,
	    node->mTransformation.d4);
    }
#ifndef NDEBUG
    for (auto& [node, entity] : nodeMap) {
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<VertArrayComponent>(entity), "Entity {} not have VertArrayComponent", entt::to_integral(entity));
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<DiscreteUniforms>(entity), "Entity {} not have DiscreteUniforms,perhaps it is has no Textures", entt::to_integral(entity));
	// 一般来说，在这里没有UBOs是符合预期的，因为addUbos需要手动之后调用
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<UBOs>(entity), "Entity {} not have UBOs", entt::to_integral(entity));
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<ShaderRef>(entity), "Entity {} not have ShaderRef", entt::to_integral(entity));
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<Relationship>(entity), "Entity {} not have Relationship", entt::to_integral(entity));
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<Transform>(entity), "Entity {} not have Transform", entt::to_integral(entity));
    }
#endif
}
const aiScene* ModelImporter::loadModel(const fs::path& path) {
    const auto scene = importer.ReadFile(path.string(), aiProcess_Triangulate | aiProcess_GenNormals);
    this->logger = RLLogger::getInstance();
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
	logger->error("Failed to load model: {}, current work path is {}", path.string(), fs::current_path().string());
	throw std::runtime_error("Failed to load model: " + path.string());
    }
    return scene;
}

glm::mat4 ModelImporter::aiMatrix4x4ToGlm(const aiMatrix4x4& from) {
    glm::mat4 to;
    to[0][0] = from.a1;
    to[1][0] = from.a2;
    to[2][0] = from.a3;
    to[3][0] = from.a4;
    to[0][1] = from.b1;
    to[1][1] = from.b2;
    to[2][1] = from.b3;
    to[3][1] = from.b4;
    to[0][2] = from.c1;
    to[1][2] = from.c2;
    to[2][2] = from.c3;
    to[3][2] = from.c4;
    to[0][3] = from.d1;
    to[1][3] = from.d2;
    to[2][3] = from.d3;
    to[3][3] = from.d4;
    return to;
}
void processAndTransformVertex(
    std::vector<GLfloat>& outVertexData,
    aiMesh* mesh,
    size_t aiVertexIdx,
    const glm::mat4& transform,
    const glm::mat3& normalTransform) {
    // 1. 获取局部坐标
    const auto& aiPos = mesh->mVertices[aiVertexIdx];
    glm::vec3 localPos(aiPos.x, aiPos.y, aiPos.z);

    //  应用变换，得到世界坐标
    glm::vec3 worldPos = transform * glm::vec4(localPos, 1.0f);

    // 2. 获取局部法线
    if (!mesh->HasNormals()) {
	throw std::runtime_error("Mesh has no normals, cannot merge.");
    }
    const auto& aiNormal = mesh->mNormals[aiVertexIdx];
    glm::vec3 localNormal(aiNormal.x, aiNormal.y, aiNormal.z);

    // 应用法线变换，得到世界空间法线
    glm::vec3 worldNormal = glm::normalize(normalTransform * localNormal);

    // 3. 获取UV坐标 (UV不需要变换)
    glm::vec2 texCoord(0.0f, 0.0f);
    if (mesh->mTextureCoords[0]) {
	texCoord.x = mesh->mTextureCoords[0][aiVertexIdx].x;
	texCoord.y = mesh->mTextureCoords[0][aiVertexIdx].y;
    }

    // 4. 将变换后的数据添加到输出vector
    outVertexData.insert(outVertexData.end(), {worldPos.x, worldPos.y, worldPos.z,
						  texCoord.x, texCoord.y,
						  worldNormal.x, worldNormal.y, worldNormal.z});
}
void ModelImporter::mergeNodeDFS(Mesh& outMesh) {
    if (!scene || !scene->mRootNode) {
	return;
    }

    std::stack<std::pair<aiNode*, glm::mat4>> nodeStack;
    nodeStack.push({scene->mRootNode, glm::mat4(1.0f)});

    while (!nodeStack.empty()) {
	auto [currentNode, parentTransform] = nodeStack.top();
	nodeStack.pop();

	glm::mat4 currentTransform = parentTransform * aiMatrix4x4ToGlm(currentNode->mTransformation);
	glm::mat3 normalTransform = glm::transpose(glm::inverse(glm::mat3(currentTransform)));

	// 遍历当前节点的所有网格
	for (size_t i = 0; i < currentNode->mNumMeshes; ++i) {
	    aiMesh* mesh = scene->mMeshes[currentNode->mMeshes[i]];

	    // 记录合并前 outMesh 的顶点数，作为当前 mesh 顶点的基址
	    unsigned int baseVertexIndex = outMesh.getVertexCount();

	    // 遍历当前网格的所有顶点，进行变换并添加到 outMesh
	    for (size_t j = 0; j < mesh->mNumVertices; ++j) {
		std::vector<GLfloat> transformedVertex;
		processAndTransformVertex(transformedVertex, mesh, j, currentTransform, normalTransform);
		outMesh.pushVertex(transformedVertex);
	    }

	    // 遍历当前网格的所有面，修正索引并添加到 outMesh
	    for (size_t j = 0; j < mesh->mNumFaces; ++j) {
		const auto& face = mesh->mFaces[j];
		for (size_t k = 0; k < face.mNumIndices; ++k) {
		    outMesh.pushIndex(baseVertexIndex + face.mIndices[k]);
		}
	    }
	}

	// 将子节点压入栈中以便继续遍历
	for (int i = currentNode->mNumChildren - 1; i >= 0; --i) {
	    nodeStack.push({currentNode->mChildren[i], currentTransform});
	}
    }
	
}

std::unique_ptr<Mesh> ModelImporter::importAsSingleMesh() {
    auto mergedMesh = std::make_unique<Mesh>(
	FloatDescs{FloatDesc{"inPos", 3}, FloatDesc{"inUV", 2}, FloatDesc{"inNormal", 3}},
	0  // 初始顶点数为0
    );

    // 调用合并逻辑
    mergeNodeDFS(*mergedMesh);

    // 为合并后的网格设置材质（使用第一个网格的材质作为代表）
    // 注意：这是一个设计简化，假设所有合并的网格共享相似的材质。
    if (scene->HasMeshes() && scene->mMeshes[0]->mMaterialIndex >= 0) {
	auto [materialData, pbrComponent] = processMaterial(scene->mMeshes[0]->mMaterialIndex);
	mergedMesh->setMaterial(materialData);
	mergedMesh->setPBRComponent(pbrComponent);
    } else {
	logger->warn("Merged mesh has no material assigned.");
    }
	
    return mergedMesh;
}

std::tuple<std::shared_ptr<MaterialData>, PBRComponent> ModelImporter::processMaterial(size_t assimpID) {
    aiMaterial* material = scene->mMaterials[assimpID];

    std::shared_ptr<MaterialData> materialData = std::make_shared<MaterialData>();
    {
	bool isTransparent = false;
	float opacity = 1.0f;
	if (material->Get(AI_MATKEY_OPACITY, opacity) != aiReturn_SUCCESS) [[unlikely]] {
	    logger->error("Failed to get opacity from material with ID: {}", assimpID);
	}

	if (opacity < 0.999f) {
	    isTransparent = true;
	}
	materialData->setTransparent(isTransparent);
    }

    aiColor4D baseColorFactorAI;
    PBRComponent pbrComponent;

    if (material->Get(AI_MATKEY_BASE_COLOR, baseColorFactorAI) == AI_SUCCESS) {
	pbrComponent.baseColorFactor = glm::vec4(baseColorFactorAI.r, baseColorFactorAI.g, baseColorFactorAI.b, baseColorFactorAI.a);

	pbrComponent.isEmpty = false;

	if (baseColorFactorAI.a < 0.999f) {
	    materialData->setTransparent(true);
	}
    }

    auto appendTexture = [this, &materialData](TextureUsageType type, aiMaterial* material) {
	aiTextureType textureType = aiTextureType_NONE;
	switch (type) {
	case TextureUsageType::DIFFUSE:
	    textureType = aiTextureType_DIFFUSE;
	    break;
	case TextureUsageType::NORMAL:
	    textureType = aiTextureType_NORMALS;
	    break;
	case TextureUsageType::SPECULAR:
	    textureType = aiTextureType_SPECULAR;
	    break;
	case TextureUsageType::AMBIENT:
	    textureType = aiTextureType_AMBIENT;
	    break;
	}
	const int textureCount = material->GetTextureCount(textureType);
	for (int i = 0; i < textureCount; i++) {
	    if (scene->mNumTextures == 0) {
		aiString texturePath;
		material->GetTexture(textureType, i, &texturePath);
		fs::path texturePathStr = this->modelRootPath / fs::path((texturePath.C_Str()));  // 来自于assimp的texture路径很可能是相对路径。而相对路径和进程的当前路径可能往往不一样,所以需要拼接模型根路径和纹理路径。这里假设纹理文件和模型文件在同一目录下，或者在子目录中。如果不在同一目录下，可能需要额外的配置来指定纹理文件的位置。
		auto texture = textureCache.getTexture(texturePathStr, type);
		materialData->appendTexture(texture);
	    } else {
		const aiTexture* embeddedTexture = scene->mTextures[i];
		auto texture = textureCache.getTexture(embeddedTexture, type);
		materialData->appendTexture(texture);
	    }
	}
    };
    appendTexture(TextureUsageType::DIFFUSE, material);
    appendTexture(TextureUsageType::NORMAL, material);
    appendTexture(TextureUsageType::SPECULAR, material);
    appendTexture(TextureUsageType::AMBIENT, material);

    return std::tuple<std::shared_ptr<MaterialData>, PBRComponent>(materialData, pbrComponent);
}
size_t ModelImporter::getNodeCount() const {
    return scene->mRootNode->mNumChildren;
}
ModelImporter::ModelImporter(const fs::path& path) : importer{}, scene(loadModel(path)), modelRootPath(path.parent_path()) {
    this->logger = RLLogger::getInstance();
}

glcore::Transform decomposeTransform(const aiMatrix4x4& transform) {
    glm::quat quaternion;
    glm::vec3 position;
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::vec3 scale;

    glm::mat4 glmTransform(
	transform.a1, transform.b1, transform.c1, transform.d1,	 // Col 1
	transform.a2, transform.b2, transform.c2, transform.d2,	 // Col 2
	transform.a3, transform.b3, transform.c3, transform.d3,	 // Col 3
	transform.a4, transform.b4, transform.c4, transform.d4	 // Col 4
    );

    if (!glm::decompose(glmTransform, scale, quaternion, position, skew, perspective)) {
	// 如果分解失败 (例如，矩阵是奇异的或包含非仿射变换，如纯投影)
	auto logger = RLLogger::getInstance();
	logger->error("glm::decompose failed for matrix!");
	throw std::invalid_argument("glm::decompose failed for matrix!");
    }

    glm::vec3 eulerAngleRad = glm::eulerAngles(quaternion);

    auto logger = RLLogger::getInstance();
    logger->trace("Input aiMatrix4x4: a4={}, b4={}, c4={}", transform.a4, transform.b4, transform.c4);	// 打印原始位移部分
    logger->trace("Decomposed: Position: {}, Euler Angle (Rad): {}, Scale: {}", glm::to_string(position), glm::to_string(eulerAngleRad), glm::to_string(scale));

    // Transform 类期望角度
    return Transform(position, glm::degrees(eulerAngleRad), scale);
}
void ModelImporter::addUbos(UBOs ubos) {
    if (this->nodeMap.size() == 0) {
	logger->error("entity map is empty");
	throw std::logic_error("entity map is empty");
    }

    for (auto [node, entity] : nodeMap) {
	singleReg->emplace_or_replace<UBOs>(entity, ubos);
    }
}
}  // namespace io
}  // namespace RGL
