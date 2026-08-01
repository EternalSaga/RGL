module;

#include <filesystem>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

export module schema_loader;

import rlstd;

namespace RGL {

export struct SchemaField {
    std::string name;
    std::string type;
    int rowCount = 0;
    int columnCount = 0;
};

export struct SchemaPushConst {
    std::string name;
    std::vector<SchemaField> fields;
};

export struct SchemaDescSlot {
    std::string name;
    int index = -1;
    std::string shape;
    int heapSlot = -1;
    std::string cppName;
};

export struct Schema {
    std::vector<SchemaPushConst> pushConstants;
    std::vector<SchemaDescSlot> descriptorSlots;
};

export inline bool has(const nlohmann::json &obj, std::string_view key) {
    return obj.contains(key);
}

export inline const char *getString(const nlohmann::json &obj, std::string_view key) {
    if (!has(obj, key)) return nullptr;
    return obj[key].get_ref<const std::string &>().c_str();
}

export Schema loadSchema(const std::filesystem::path &luaPath) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
    lua["dofile"] = sol::nil;
    lua["loadfile"] = sol::nil;
    lua["package"] = sol::nil;
    lua["require"] = sol::nil;

    auto result = lua.do_file(luaPath.string().c_str());
    if (!result.valid()) {
        throw std::runtime_error("Failed to load schema: " + luaPath.string());
    }

    sol::table rootTable = result[0].as<sol::table>();

    Schema schema;

    sol::table pcArray = rootTable["pushConstants"];
    if (pcArray.valid() && pcArray.is<sol::table>()) {
        for (int i = 1; i <= static_cast<int>(pcArray.size()); ++i) {
            SchemaPushConst pc;
            sol::table pcItem = pcArray[i];
            sol::optional<std::string> nameOpt = pcItem["name"];
            if (nameOpt) pc.name = *nameOpt;

            sol::table fieldsArray = pcItem["fields"];
            if (fieldsArray.valid() && fieldsArray.is<sol::table>()) {
                for (int fi = 1; fi <= static_cast<int>(fieldsArray.size()); ++fi) {
                    SchemaField f;
                    sol::table fObj = fieldsArray[fi];
                    sol::optional<std::string> fname = fObj["name"];
                    if (fname) f.name = *fname;
                    sol::optional<std::string> ftype = fObj["type"];
                    if (ftype) f.type = *ftype;
                    sol::optional<int> rc = fObj["rowCount"];
                    if (rc) f.rowCount = *rc;
                    sol::optional<int> cc = fObj["columnCount"];
                    if (cc) f.columnCount = *cc;
                    pc.fields.push_back(std::move(f));
                }
            }
            schema.pushConstants.push_back(std::move(pc));
        }
    }

    sol::table dsArray = rootTable["descriptorSlots"];
    if (dsArray.valid() && dsArray.is<sol::table>()) {
        for (int i = 1; i <= static_cast<int>(dsArray.size()); ++i) {
            SchemaDescSlot ds;
            sol::table dsItem = dsArray[i];
            sol::optional<std::string> nameOpt = dsItem["name"];
            if (nameOpt) ds.name = *nameOpt;
            sol::optional<int> idx = dsItem["index"];
            if (idx) ds.index = *idx;
            sol::optional<std::string> shape = dsItem["shape"];
            if (shape) ds.shape = *shape;
            sol::optional<int> hs = dsItem["heapSlot"];
            if (hs) ds.heapSlot = *hs;
            sol::optional<std::string> cppName = dsItem["cppName"];
            if (cppName) ds.cppName = *cppName;
            schema.descriptorSlots.push_back(std::move(ds));
        }
    }

    return schema;
}

}  // namespace RGL
