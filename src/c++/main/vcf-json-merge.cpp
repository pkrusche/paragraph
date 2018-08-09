// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Copyright (c) 2017 Illumina, Inc.
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:

// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.

// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/**
 * \brief Merge JSON and VCF files
 *
 * \file vcf-json-merge.cpp
 * \author Peter Krusche
 * \email pkrusche@gmail.com
 *
 */

#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "json/json.h"
#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "common/JsonHelpers.hh"
#include "idxdepth/DepthEstimation.hh"
#include "idxdepth/IndexBinning.hh"
#include "idxdepth/Parameters.hh"

#include "common/Error.hh"

namespace po = boost::program_options;

using idxdepth::Parameters;
using std::cerr;
using std::endl;
using std::string;

int main(int argc, char const* argv[])
{
    po::options_description desc("Allowed options");
    // clang-format off
    desc.add_options()
        ("help,h", "produce help message")
        ("input-vcf,v", po::value<string>(), "Input VCF file. Must contain GRMPY_ID field to allow matching of records.")
        ("input-json,j", po::value<string>(), "Input JSON file, must be output from grmpy.")
        ("output,o", po::value<string>(), "Output file name. Will output to stdout if omitted.")
        ("reference,r", po::value<string>(), "FASTA with reference genome")
        ("log-level", po::value<string>()->default_value("info"), "Set log level (error, warning, info).")
        ("log-file", po::value<string>()->default_value(""), "Log to a file instead of stderr.")
        ("log-async", po::value<bool>()->default_value(true), "Enable / disable async logging.");
    // clang-format on

    po::variables_map vm;
    std::shared_ptr<spdlog::logger> logger;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if (vm.empty() || (vm.count("help") != 0u))
        {
            cerr << desc << endl;
            return 1;
        }

        initLogging(
            "idxdepth", vm["log-file"].as<string>().c_str(), vm["log-async"].as<bool>(),
            vm["log-level"].as<string>().c_str());
        logger = LOG();

        string vcf_path;
        if (vm.count("input-vcf") != 0u)
        {
            vcf_path = vm["input-vcf"].as<string>();
            logger->info("VCF: {}", vcf_path);
            assertFileExists(vcf_path);
        }
        else
        {
            error("ERROR: VCF File with variants is missing.");
        }

        string json_path;
        if (vm.count("input-json") != 0u)
        {
            json_path = vm["input-json"].as<string>();
        }
        else
        {
            error("ERROR: JSON File with variants is missing.");
        }

        string reference_path;
        if (vm.count("reference") != 0u)
        {
            reference_path = vm["reference"].as<string>();
            logger->info("Reference: {}", reference_path);
            assertFileExists(reference_path);
        }
        else
        {
            error("ERROR: Reference genome is missing.");
        }

        string output_path;
        if (vm.count("output") != 0u)
        {
            output_path = vm["output"].as<string>();
            logger->info("Output path: {}", output_path);
        }

        if (output_path.empty())
        {
            output_path = "-";
        }
        else
        {
            std::ofstream output_stream(output_path);
            output_stream << common::writeJson(output, false);
        }
    }
    catch (const std::exception& e)
    {
        if (logger)
        {
            logger->critical(e.what());
        }
        else
        {
            cerr << e.what() << endl;
        }
        return 1;
    }

    return 0;
}
