// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <fstream>
#include <regex>
#include <chrono>
#include <ctime>

#include "inference_engine.hpp"

#include "common_test_utils/file_utils.hpp"

#include "ops_cache.hpp"
#include "op_cloner.hpp"
#include "utils/model_wrap_struct.hpp"
#include "gflag_config.hpp"
#include <string.h>

static std::vector<std::regex> getRegexByFrontend() {
    std::vector<std::regex> result;
#ifdef ENABLE_OV_ONNX_FRONTEND
    result.push_back(std::regex(R"(.*\.onnx)"));
#endif
#ifdef ENABLE_OV_PADDLE_FRONTEND
    result.push_back(std::regex(R"(.*\.pdmodel)"));
#endif
#ifdef ENABLE_OV_TF_FRONTEND
    result.push_back(std::regex(R"(.*\.pb)"));
#endif
#ifdef ENABLE_OV_IR_FRONTEND
    result.push_back(std::regex(R"(.*\.xml)"));
#endif
    return result;
}

std::vector<SubgraphsDumper::Model> findModelsInDirs(const std::vector<std::string> &dirs) {
    std::vector<std::string> input_folder_content;
    for (const auto &dir : dirs) {
        if (!CommonTestUtils::directoryExists(dir)) {
            std::string msg = "Input directory (" + dir + ") doesn't not exist!";
            throw std::runtime_error(msg);
        }
        const auto patterns = getRegexByFrontend();
        const auto content = CommonTestUtils::getFileListByPatternRecursive(dirs, patterns);
        input_folder_content.insert(input_folder_content.end(), content.begin(), content.end());
    }
    std::vector<SubgraphsDumper::Model> models;
    auto xml_regex = std::regex(FLAGS_path_regex);
    for (const auto &file : input_folder_content) {
        if (std::regex_match(file, xml_regex)) {
            models.emplace_back(SubgraphsDumper::Model(file));
        }
    }
    std::sort(models.begin(), models.end());
    std::reverse(models.begin(), models.end());
    if (!CommonTestUtils::directoryExists(FLAGS_output_folder)) {
        std::string msg = "Output directory (" + FLAGS_output_folder + ") doesn't not exist!";
        throw std::runtime_error(msg);
    }
    return models;
}

void cacheModels(std::unique_ptr<SubgraphsDumper::OPCache> &cache,
                 uint8_t& ret_code,
                 const std::vector<SubgraphsDumper::Model>& models,
                 const bool extract_body) {
    auto core = ov::test::utils::PluginCache::get().core();
    time_t rawtime;
    struct tm *timeinfo;
    char buffer[20];
    size_t all_models = models.size();
    for (size_t i = 0; i < all_models; ++i) {
        const auto model = models[i];
        if (CommonTestUtils::fileExists(model.path)) {
            try {
                time(&rawtime);
                timeinfo = localtime(&rawtime);  // NOLINT no localtime_r in C++11

                strftime(buffer, 20, "%H:%M:%S", timeinfo);
                std::cout << "[" << std::string(buffer) << "][" << i + 1 << "/" << all_models << "]Processing model: "
                          << model.path << std::endl;

                const auto function = core->read_model(model.path);
                cache->update_ops_cache(function, extract_body, model.path);
            } catch (std::exception &e) {
                std::cout << "Model processing failed with exception:" << std::endl << e.what() << std::endl;
                ret_code = 1;
                continue;
            }
        }
    }
}


int main(int argc, char *argv[]) {
    uint8_t ret_code = 0;

    gflags::ParseCommandLineNonHelpFlags(&argc, &argv, true);
    if (FLAGS_h) {
        showUsage();
        return 0;
    }
    SubgraphsDumper::ClonersMap::constant_size_threshold_mb = FLAGS_constants_size_threshold;

    std::vector<std::string> local_cache_dirs = CommonTestUtils::splitStringByDelimiter(FLAGS_local_cache);
    std::vector<std::string> dirs = CommonTestUtils::splitStringByDelimiter(FLAGS_input_folders);
    auto models = findModelsInDirs(dirs);

    auto cache = SubgraphsDumper::OPCache::make_cache();
    if (!FLAGS_local_cache.empty()) {
        auto cachedOps = findModelsInDirs(local_cache_dirs);
        cacheModels(cache, ret_code, cachedOps, FLAGS_extract_body);
    }
    cacheModels(cache, ret_code, models, FLAGS_extract_body);
    cache->serialize_cached_ops(FLAGS_output_folder);

    return ret_code;
}
