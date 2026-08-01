#include <boost/program_options.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <unistd.h>

import rllogger;
import schema_loader;
import validator;
import generator;

namespace po = boost::program_options;
namespace fs = std::filesystem;
using json = nlohmann::json;

static void printUsage() {
    auto *logger = RGL::RLLogger::getInstance();
    logger->info("Usage:");
    logger->info("  reflection_tool validate  --json <path> --schema <path>");
    logger->info("  reflection_tool generate  --json <path> --schema <path> --output <path> --module-name <name> --template <path>");
}

int main(int argc, char *argv[]) {
    RGL::RLLogger::initialize(LOG_CONFIG_PATH);
    auto *logger = RGL::RLLogger::getInstance();
    logger->debug("reflection_tool started, pid={}", ::getpid());

    try {
        std::string command;
        std::string jsonPath, schemaPath, outputPath, moduleName, templatePath;

        po::options_description desc("Options");
        desc.add_options()("help,h", "produce help message")
            ("json,j", po::value<std::string>(&jsonPath)->required(), "path to slangc reflection JSON")
            ("schema,s", po::value<std::string>(&schemaPath), "path to Lua schema file")
            ("output,o", po::value<std::string>(&outputPath), "output .cppm file path")
            ("module-name,m", po::value<std::string>(&moduleName), "C++ module name for generated .cppm")
            ("template,t", po::value<std::string>(&templatePath), "path to inja template file");

        if (argc < 2) {
            printUsage();
            return 1;
        }

        command = argv[1];
        int cmdArgc = argc - 1;
        char *cmdArgv[1024];
        std::ranges::copy_n(argv + 1, cmdArgc, cmdArgv);

        po::variables_map vm;
        po::store(po::command_line_parser(cmdArgc, cmdArgv).options(desc).run(), vm);
        po::notify(vm);

        if (vm.count("help")) {
            printUsage();
            return 0;
        }

        if (!fs::exists(jsonPath)) {
            logger->error("Reflection JSON not found: {}", jsonPath);
            return 1;
        }

        std::ifstream jsonFile(jsonPath);
        json reflection = json::parse(jsonFile);
        logger->debug("Loaded reflection JSON: {} parameters", reflection["parameters"].size());

        if (command == "validate") {
            if (vm["schema"].empty()) {
                logger->error("--schema is required for 'validate'");
                return 1;
            }
            auto schema = RGL::loadSchema(schemaPath);
            bool ok = RGL::validateSchema(reflection, schema);
            if (ok) {
                logger->info("[PASS] Schema validation passed");
                return 0;
            } else {
                logger->error("[FAIL] Schema validation failed");
                return 1;
            }

        } else if (command == "generate") {
            if (outputPath.empty()) {
                logger->error("--output is required for 'generate'");
                return 1;
            }
            if (moduleName.empty()) {
                logger->error("--module-name is required for 'generate'");
                return 1;
            }
            if (templatePath.empty()) {
                logger->error("--template is required for 'generate'");
                return 1;
            }

            RGL::Schema schema{};
            if (!vm["schema"].empty()) {
                schema = RGL::loadSchema(schemaPath);
                if (!RGL::validateSchema(reflection, schema)) {
                    logger->error("[FAIL] Schema validation failed before generation");
                    return 1;
                }
                logger->info("[PASS] Schema validation passed, generating .cppm");
            }

            std::string cppm = RGL::generateCppm(reflection, schema, templatePath, moduleName);
            std::ofstream out(outputPath);
            if (!out.is_open()) {
                logger->error("Cannot open output file: {}", outputPath);
                return 1;
            }
            out << cppm;
            logger->info("[OK] Generated {}", outputPath);
            return 0;

        } else {
            logger->error("Unknown command: {}", command);
            printUsage();
            return 1;
        }

    } catch (const po::error &e) {
        logger->error("Error: {}", e.what());
        printUsage();
        return 1;
    } catch (const std::exception &e) {
        logger->error("Error: {}", e.what());
        return 1;
    }
}
