#!/usr/bin/env python3
# coding=utf-8

#
# Copyright (c) 2017 Illumina, Inc.
# All rights reserved.
#
# This file is distributed under the simplified BSD license.
# The full text can be found here (and in the LICENSE file in the root folder of
# this distribution):
#
# https://github.com/Illumina/licenses/blob/master/Simplified-BSD-License.txt
#
# January 2018
#
# Run paragraph tools on multiple sites on multiple samples
#   This script is suitable for a small list of sites and samples.
#   To run ParaGRAPH on large-scale sites and samples, please refer to the Snakemake template at doc/multi-samples.md
# Output will be one JSON for all samples with genotyping information
#
# Use multiparagraph.py as template
#
# Usage:
#
# For usage instructions run with option --help
#
# Author:
#
# Sai Chen <schen6@illumina.com>
#

import argparse
import gzip
import itertools
import json
import logging
import multiprocessing
import os
import pipes
import subprocess
import tempfile
import traceback
import re

import findgrm  # pylint: disable=unused-import
from grm.helpers import LoggingWriter
from grm.vcf2paragraph import convert_vcf_to_json
from grm.graph_templates import make_graph


def load_graph_description(args):
    """
    load graph description from either vcf or json
    """
    event_list = []
    extension = os.path.splitext(args.input)[1]
    if extension == ".gz":
        file_type = os.path.splitext(os.path.splitext(args.input)[0])[1]
        extension = file_type + ".gz"
    if extension == ".vcf" or extension == ".vcf.gz":
        logging.info("Input is a vcf. Converting to JSON with graph description...")
        try:
            converted_json_path = os.path.join(args.output, "variants.json.gz")
            event_list = convert_vcf_to_json(args.input, args.reference, read_length=args.read_length,
                                             max_ref_node_length=args.max_ref_node_length,
                                             graph_type=args.graph_type,
                                             split_type=args.split_type,
                                             retrieve_ref_sequence=args.retrieve_reference_sequence,
                                             threads=args.threads)
            with gzip.open(converted_json_path, "wt") as converted_json_file:
                json.dump(event_list, converted_json_file, sort_keys=True, indent=4, separators=(',', ': '))
        except Exception:  # pylint: disable=W0703
            logging.error("VCF to JSON conversion failed.")
            traceback.print_exc(file=LoggingWriter(logging.ERROR))
            raise
        logging.info("Done. Graph Json stored at %s", converted_json_path)
        args.input = converted_json_path
    elif extension == ".json" or extension == "json.gz":
        if extension == ".json":
            json_file = open(args.input, 'r')
        else:
            json_file = gzip.open(args.input, 'r')
        event_list = json.load(json_file)
        num_converted_event = 0
        # if JSON has no graph description
        for event in event_list:
            if "graph" not in event:
                if "nodes" not in event and "edges" not in event:
                    try:
                        event["type"], event["graph"] = make_graph(args.reference, event)
                    except Exception:  # pylint: disable=W0703
                        logging.error("Fail to make graph for JSON event.")
                        traceback.print_exc(file=LoggingWriter(logging.ERROR))
                        raise
                    num_converted_event += 1
        if num_converted_event:
            logging.info("Constructed graph for %d events in JSON.", num_converted_event)
        json_file.close()
    else:
        raise Exception("Unknown input file extension %s for %s. Only VCF or JSON is allowed!" %
                        (extension, args.input))
    return event_list


def run_grmpy_single_variant(event_and_args):
    """
    Run grmpy for one single variant on all samples with multiple threads
    return grmpy result as python dict object
    """
    event = event_and_args[0]
    args = event_and_args[1]
    tempfiles = []
    exception = ""
    error_log = ""
    error = False
    gt_result = {}

    try:
        input_json_file = tempfile.NamedTemporaryFile(dir=args.scratch_dir, mode="wt", suffix=".json", delete=False)
        tempfiles.append(input_json_file.name)
        if "graph" in event:
            json.dump(event["graph"], input_json_file, indent=4, separators=(',', ': '))
        else:
            json.dump(event, input_json_file, indent=4, separators=(',', ': '))
        input_json_file.close()
        error_log = input_json_file.name + ".output.log"
        tempfiles.append(error_log)

        grmpy_out_path = input_json_file.name + ".grmpy.json"
        tempfiles.append(grmpy_out_path)
        commandline = args.grmpy

        commandline += " -r %s" % pipes.quote(args.reference)
        commandline += " -m %s" % pipes.quote(args.manifest)
        commandline += " -g %s" % pipes.quote(input_json_file.name)
        commandline += " -o %s" % pipes.quote(grmpy_out_path)
        if args.use_em:
            commandline += " --useEM"
        commandline += " --log-file %s" % pipes.quote(error_log)
        o = subprocess.check_output(commandline, shell=True, stderr=subprocess.STDOUT)

        try:
            o = o.decode("utf-8")
        except:  # pylint: disable=bare-except
            o = str(o)

        for line in o.split("\n"):
            if line:
                logging.warning(line)

        with open(grmpy_out_path, "rt") as grmpy_out_file:
            gt_result = json.load(grmpy_out_file)

    except Exception:  # pylint: disable=broad-except
        logging.error("Exception when running grmpy on %s", str(event))
        logging.error('-' * 60)
        traceback.print_exc(file=LoggingWriter(logging.ERROR))
        logging.error('-' * 60)
        exception = traceback.format_exc()
        error = True
    except BaseException:  # pylint: disable=broad-except
        logging.error("Exception when running grmpy on %s", str(event))
        logging.error('-' * 60)
        traceback.print_exc(file=LoggingWriter(logging.ERROR))
        logging.error('-' * 60)
        exception = traceback.format_exc()
        error = True
    finally:
        if error:
            try:
                with open(error_log, "rt") as f:
                    for line in f:
                        line = str(line).strip()
                        logging.error(line)
            except:  # pylint: disable=bare-except
                pass
            gt_result["error"] = {
                "exception": exception
            }
        if not args.keep_scratch:
            for f in tempfiles:
                try:
                    os.remove(f)
                except:  # pylint: disable=bare-except
                    pass
    return gt_result


def make_argument_parser():
    """
    :return: an argument parser
    """
    parser = argparse.ArgumentParser("Multigrmpy.py")

    parser.add_argument("-i", "--input", help="Input file of variants. Must be either JSON or VCF.",
                        type=str, dest="input", required=True)

    parser.add_argument("-m", "--manifest", help="Manifest of samples with path and bam stats.",
                        type=str, dest="manifest", required=True)

    parser.add_argument("-o", "--output", help="Output directory.", type=str, dest="output", required=True)

    parser.add_argument("-r", "--reference-sequence", help="Reference genome fasta file.",
                        type=str, dest="reference", required=True)

    parser.add_argument("--event-threads", "-t", dest="threads", type=int, default=multiprocessing.cpu_count(),
                        help="Number of events to process in parallel.")

    parser.add_argument("--keep-scratch", dest="keep_scratch", default=None, action="store_true",
                        help="Do not delete temp files.")

    parser.add_argument("--scratch-dir", dest="scratch_dir", default=None,
                        help="Directory for temp files")

    parser.add_argument("--grmpy", dest="grmpy", default=os.path.join(findgrm.GRM_BASE, "bin", "grmpy"),
                        type=str, help="Path to the grmpy executable")

    parser.add_argument("--logfile", dest="logfile", default=None,
                        help="Write logging information into file rather than to stderr")

    verbosity_options = parser.add_mutually_exclusive_group(required=False)

    verbosity_options.add_argument("--verbose", dest="verbose", default=False, action="store_true",
                                   help="Raise logging level from warning to info.")

    verbosity_options.add_argument("--quiet", dest="quiet", default=False, action="store_true",
                                   help="Set logging level to output errors only.")

    stat_options = parser.add_mutually_exclusive_group(required=False)

    stat_options.add_argument("-G", "--genotyping-parameters", dest="genotyping-parameters", default="",
                              type=str, help="JSON string or file with genotyping model parameters.")

    stat_options.add_argument("--useEM", dest="use_em", default=False, action="store_true",
                              help="Use Expectation-Maximization algorithm in genotyping. Slower but more accurate.")

    vcf2json_options = parser.add_mutually_exclusive_group(required=False)

    vcf2json_options.add_argument("--vcf-split", default="lines", dest="split_type", choices=["lines", "full", "by_id"],
                                  help="Mode for splitting the input VCF: lines (default) -- one graph per record ;"
                                  " full -- one graph for the whole VCF ;"
                                  " by_id -- use the VCF id column to group adjacent records")
    vcf2json_options.add_argument("-p", "--read-length", dest="read_length", default=150, type=int,
                                  help="Read length -- this can be used to add reference padding for disambiguation.")

    vcf2json_options.add_argument("-l", "--max-ref-node-length", dest="max_ref_node_length", type=int, default=1000,
                                  help="Maximum length of reference nodes before they get padded and truncated.")

    vcf2json_options.add_argument("--retrieve-reference-sequence", help="Retrieve reference sequence for REF nodes",
                                  action="store_true", dest="retrieve_reference_sequence", default=False)

    vcf2json_options.add_argument("--graph-type", choices=["alleles", "haplotypes"], default="alleles", dest="graph_type",
                                  help="Type of complex graph to generate. Same as --graph-type in vcf2paragraph.")

    return parser


def run(args):
    """
    :run the wrapper
    """
    if args.verbose:
        loglevel = logging.INFO
    elif args.quiet:
        loglevel = logging.ERROR
    else:
        loglevel = logging.WARNING

    if not os.path.isdir(args.output):
        os.makedirs(args.output, exist_ok=True)

    # set default log file
    if not args.logfile:
        args.logfile = os.path.join(args.output, "GraphTyping.log")

    # reinitialize logging
    for handler in logging.root.handlers[:]:
        logging.root.removeHandler(handler)
    logging.basicConfig(filename=args.logfile, format='%(asctime)s %(levelname)-8s %(message)s', level=loglevel)

    # check format of manifest
    with open(args.manifest) as manifest_file:
        headers = {"id": False, "path": False, "idxdepth": False, "depth": False, "read length": False}
        for line in manifest_file:
            if line.startswith("#"):
                line = line[1:]
            line = line.strip()
            fields = re.split('\t|,', line)
            for field in fields:
                if field not in headers:
                    header_str = ""
                    for h in headers:
                        header_str += h + ","
                    raise Exception("Illegal header name %s. Allowed headers:\n%s" % (field, header_str))
                headers[field] = True
            if not headers["id"] or not headers["path"]:
                raise Exception("Missing header \"id\" or \"path\" in manifest")
            if not headers["idxdepth"]:
                if not headers["depth"] or not headers["read length"]:
                    raise Exception("Missing header \"idxdepth\", or \"depth\" and \"read length\" in manifest.")
            break

    # prepare input graph description
    try:
        event_list = load_graph_description(args)
    except Exception:  # pylint: disable=W0703
        traceback.print_exc(file=LoggingWriter(logging.ERROR))
        raise

    # run grmpy
    with multiprocessing.Pool(args.threads) as pool:
        results = pool.map(run_grmpy_single_variant, zip(event_list, itertools.repeat(args)))

    logging.info("Finished genotyping. Merging output...")
    result_json_path = os.path.join(args.output, "genotypes.json.gz")
    with gzip.open(result_json_path, "wt") as result_json_file:
        json.dump(results, result_json_file, sort_keys=True, indent=4, separators=(',', ':'))
        logging.info("ParaGRAPH completed on %d sites.", len(event_list))


def main():
    parser = make_argument_parser()
    args = parser.parse_args()
    run(args)


if __name__ == '__main__':
    main()
