#pragma once
#include <cstddef>
#include <glm/ext/matrix_transform.hpp>
#include <glm/fwd.hpp>
#include <glm/glm.hpp>
#include <boost/uuid/uuid.hpp>
#include <map>
#include <memory>
#include <string>
#include "GLObj.hpp"
#include "Light.hpp"
#include "Mesh.hpp"
#include "Material.hpp"
#include "Shader.hpp"
namespace RGL {
namespace glcore {

class Entity {
    glm::vec3 position;

    float angleX;
    float angleY;
    float angleZ;
    glm::vec3 scale;
    boost::uuids::uuid uuid;
    std::string name;
    std::unique_ptr<Mesh> mesh;
    std::unique_ptr<Material> material;
    std::shared_ptr<Shader> shader;
    VAOCreater vaoCreator;

    std::unique_ptr<VAO> vao;
    size_t mvertCount;
    size_t midxOffset;

   protected:
    virtual void setShaderUniforms(Shader* shader) {
    }

   public:
    Entity(glm::vec3 position, float angleX, float angleY, float angleZ, glm::vec3 scale, std::shared_ptr<Shader> shader, std::string name);
    void rotateX(float angle);
    void rotateY(float angle);
    void rotateZ(float angle);

    void setPosition(const glm::vec3& position);
    void setScale(const glm::vec3& scale);
    boost::uuids::uuid getUUID();

    void setMesh(std::unique_ptr<Mesh> mesh);
    void setMaterial(std::unique_ptr<Material> material);

    Mesh* getMesh();
    // pitch yaw roll
    glm::mat4 getModelMatrix();

    void draw();
};

class SceneManager {
   private:
    std::map<boost::uuids::uuid, std::unique_ptr<Entity>> entities;
    std::map<boost::uuids::uuid, std::unique_ptr<Light>> lights;

    VAOCreater vaoCreator;
    std::shared_ptr<Shader> shader;

   public:
    SceneManager(std::shared_ptr<Shader> shader);
    void addEntity(std::unique_ptr<Entity> entity);

    void removeEntity(const boost::uuids::uuid& uuid);
    void addLight(std::unique_ptr<Light> light);

    void drawALL();
};

}  // namespace glcore
}  // namespace RGL
