#pragma once
#include <string>
#include <vector>

struct ValidationError {
    std::string file;
    std::string message;
};

// Validates all data/* JSON files against their schemas at startup.
// Logs all errors and returns false if any critical errors were found.
class DataValidator {
public:
    // Run full validation of all data directories under root_path
    bool validate_all(const std::string& root_path);

    bool validate_file(const std::string& path, const std::string& schema_type);

    const std::vector<ValidationError>& errors() const { return m_errors; }
    void clear_errors() { m_errors.clear(); }

private:
    bool validate_voxel_type(const std::string& path);
    bool validate_item_type (const std::string& path);
    bool validate_species   (const std::string& path);
    bool validate_game_mode (const std::string& path);
    bool validate_reaction  (const std::string& path);

    void add_error(const std::string& file, const std::string& msg);

    std::vector<ValidationError> m_errors;
};
