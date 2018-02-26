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

#include "common/ReadExtraction.hh"
#include "common/Error.hh"
#include <cstdlib>
#include <list>

namespace common
{

/**
 * High-level read extraction interface
 * @param reader An open bam reader
 * @param target_regions list of target regions
 * @param max_reads maximum number of reads per target region to retrieve
 * @param all_reads output vector to store retrieved reads
 * @param avr_fragment_length decides how long to extend beyond target region
 */
void extractReads(
    BamReader& reader, std::list<Region> const& target_regions, int max_num_reads, std::vector<p_Read>& all_reads,
    int avr_fragment_length)
{
    auto logger = LOG();
    for (const auto& region : target_regions)
    {
        logger->info("[Retrieving for region {}.]", (std::string)region);
        std::pair<int, int> num_extracted_reads
            = extractReadsFromRegion(all_reads, max_num_reads, reader, region, avr_fragment_length);

        if (max_num_reads == num_extracted_reads.first)
        {
            logger->warn("Reached maximum number of reads ({}).", max_num_reads);
        }
        else
        {
            logger->info("[Retrieved {} + {} additional reads]", num_extracted_reads.first, num_extracted_reads.second);
        }
    }
}

/**
 * High-level read extraction interface
 * @param bam_path path to BAM file
 * @param reference_path path to FASTA reference file
 * @param target_regions list of target regions
 * @param max_reads maximum number of reads per target region to retrieve
 * @param all_reads output vector to store retrieved reads
 * @param avr_fragment_length decides how long to extend beyond target region
 */
void extractReads(
    const std::string& bam_path, const std::string& reference_path, std::list<Region> const& target_regions,
    int max_num_reads, std::vector<p_Read>& all_reads, int avr_fragment_length)
{
    auto logger = LOG();
    logger->info("Retrieving reads from {}", bam_path);
    BamReader reader(bam_path, reference_path);
    extractReads(reader, target_regions, max_num_reads, all_reads, avr_fragment_length);
    logger->info("Done retrieving reads from {}", bam_path);
}

/**
 * Lower-level read extraction interface for specified target region
 * @return <num_original_extracted, num_recovered_mates> when finish
 * @param output vector to store retrieve reads
 * @param max_reads maximum number of reads to load
 * @param reader Reader that will provide the reads
 * @param region target region
 * @param avr_fragment_length decides how long to extend beyond target region
 */
std::pair<int, int> extractReadsFromRegion(
    std::vector<p_Read>& all_reads, int max_num_reads, ReadReader& reader, const Region& region,
    int avr_fragment_length)
{

    int extended_flank = avr_fragment_length * 3;
    const auto extended_region = region.getExtendedRegion(static_cast<int64_t>(extended_flank));
    reader.setRegion(extended_region);

    ReadPairs read_pairs;
    extractMappedReadsFromRegion(read_pairs, max_num_reads, reader, region);

    std::pair<int, int> num_extracted_reads;
    if (max_num_reads == read_pairs.num_reads())
    {
        num_extracted_reads = std::make_pair(read_pairs.num_reads(), 0);
    }
    else
    {
        const int num_reads_original = read_pairs.num_reads();
        recoverMissingMates(reader, read_pairs);
        const int num_reads_recovered = read_pairs.num_reads() - num_reads_original;
        num_extracted_reads = std::make_pair(num_reads_original, num_reads_recovered);
    }

    read_pairs.getReads(all_reads);
    return num_extracted_reads;
}

/**
 * Low-level read extraction for mapped reads in target region
 * @param read_pairs Container for extracted reads
 * @param reader Reader that will provide the reads
 * @param max_reads Maximum number of reads to load
 * @param region Region to check if a read is in
 */
void extractMappedReadsFromRegion(ReadPairs& read_pairs, int max_num_reads, ReadReader& reader, const Region& region)
{
    Read read;
    while ((read_pairs.num_reads() != max_num_reads) && reader.getAlign(read))
    {
        if (isReadOrItsMateInRegion(read, region))
        {
            read_pairs.add(read);
        }
    }
}

/**
 *
 * return true if this aligned read or its mate overlaps >= 1 base with the target region
 * @param read The single read to be checked
 * @param region Region to check if a read is in
 */
bool isReadOrItsMateInRegion(Read& read, const Region& region)
{
    bool in_region;
    std::string bases = read.bases();
    if (read.pos() > region.end || read.pos() + static_cast<int64_t>(bases.length()) < region.start)
    {
        in_region = false;
        if (read.chrom_id() == read.mate_chrom_id())
        {
            if (!(read.mate_pos() > region.end
                  || read.mate_pos() + static_cast<int64_t>(bases.length()) < region.start))
            {
                in_region = true;
            }
        }
    }
    else
    {
        in_region = true;
    }
    return in_region;
}

/**
 * Lower-level read extraction interface
 * @param max_reads maximum number of reads to load
 * @param reader Reader that will provide the reads
 * @param read_pairs output ReadPairs structure
 */
void recoverMissingMates(ReadReader& reader, ReadPairs& read_pairs)
{
    for (const auto& kv : read_pairs)
    {
        const ReadPair& read_pair = kv.second;

        // If a mate is missing, try to recover it.
        if (!read_pair.first_mate().is_initialized() || !read_pair.second_mate().is_initialized())
        {
            // Get references for existing Align and the one that needs to be filled in and then process
            // This is done to avoid code duplication.
            const Read& initialized_read
                = read_pair.first_mate().is_initialized() ? read_pair.first_mate() : read_pair.second_mate();

            // Do not recover nearby mates.
            const int kMaxNormalDistanceBetweenMates = 1000;
            if ((initialized_read.chrom_id() == initialized_read.mate_chrom_id())
                && (std::abs(initialized_read.pos() - initialized_read.mate_pos()) < kMaxNormalDistanceBetweenMates))
            {
                continue;
            }

            Read missing_read;
            reader.getAlignedMate(initialized_read, missing_read);
            if (missing_read.is_initialized())
            {
                read_pairs.add(missing_read);
            }
        }
    }
}
}
