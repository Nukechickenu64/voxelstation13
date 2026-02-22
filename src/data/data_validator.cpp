#include "data/data_validator.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <SDL3/SDL.h>

namespace fs = std::filesystem;
using json   = nlohmann::json;

static bool has_keys(const json& j, const std::vector<std::string>& keys,
                     const std::string& file, std::vector<ValidationError>& errors)
{
    bool ok = true;
    for (const auto& k : keys) {
        if (!j.contains(k)) {
            errors.push_back({file, "Missing required field: " + k});
            ok = false;
        }
    }
    return ok;
}

void DataValidator::add_error(const std::string& file, const std::string& msg)
{
    m_errors.push_back({file, msg});
    SDL_Log("DataValidator [%s]: %s", file.c_str(), msg.c_str());
}

bool DataValidator::validate_all(const std::string& root_path)
{
    bool ok = true;
    auto scan = [&](const std::string& sub, const std::string& type) {
        std::string dir = root_path + "/" + sub;
        if (!fs::exists(dir)) return;
        for (const auto& entry : fs::recursive_directory_iterator(dir))
            if (entry.path().extension() == ".json")
                ok &= validate_file(entry.path().string(), type);
    };

    scan("voxel_types", "voxel_type");
    scan("item_types",  "item_type");
    scan("mob_species", "species");
    scan("game_modes",  "game_mode");
    scan("reactions",   "reaction");

    return ok;
}

bool DataValidator::validate_file(const std::string& path, const std::string& schema_type)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        add_error(path, "Cannot open file");
        return false;
    }
    json j;
    try { f >> j; }
    catch (const json::exception& e) {
        add_error(path, std::string("JSON parse error: ") + e.what());
        return false;
    }

    if (schema_type == "voxel_type") return validate_voxel_type(path);
    if (schema_type == "item_type")  return validate_item_type(path);
    if (schema_type == "species")    return validate_species(path);
    if (schema_type == "game_mode")  return validate_game_mode(path);
    if (schema_type == "reaction")   return validate_reaction(path);
    return true;
}

bool DataValidator::validate_voxel_type(const std::string& path)
{
    std::ifstream f(path); json j; f >> j;
    auto validate_obj = [&](const json& obj) {
        has_keys(obj, {"id", "name"}, path, m_errors);
    };
    if (j.is_array()) for (auto& o : j) validate_obj(o);
    else validate_obj(j);
    return m_errors.empty();
}

bool DataValidator::validate_item_type(const std::string& path)
{
    std::ifstream f(path); json j; f >> j;
    auto validate_obj = [&](const json& obj) {
        has_keys(obj, {"id", "name"}, path, m_errors);
    };
    if (j.is_array()) for (auto& o : j) validate_obj(o);
    else validate_obj(j);
    return m_errors.empty();
}

bool DataValidator::validate_species(const std::string& path)
{
    std::ifstream f(path); json j; f >> j;
    has_keys(j, {"species", "slots"}, path, m_errors);
    return m_errors.empty();
}

bool DataValidator::validate_game_mode(const std::string& path)
{
    std::ifstream f(path); json j; f >> j;
    has_keys(j, {"id", "name"}, path, m_errors);
    return m_errors.empty();
}

bool DataValidator::validate_reaction(const std::string& path)
{
    std::ifstream f(path); json j; f >> j;
    has_keys(j, {"id", "reagents", "products"}, path, m_errors);
    return m_errors.empty();
}
