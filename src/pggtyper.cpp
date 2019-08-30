#include <iostream>
#include <sstream>
#include <sys/resource.h>
#include <mutex>
#include <thread>
#include <algorithm>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <boost/bind.hpp>
#include "kmercounter.hpp"
#include "emissionprobabilitycomputer.hpp"
#include "copynumber.hpp"
#include "variantreader.hpp"
#include "uniquekmercomputer.hpp"
#include "hmm.hpp"
#include "commandlineparser.hpp"
#include "timer.hpp"

using namespace std;

struct Results {
	mutex m;
	map<string, vector<GenotypingResult>> result;	
};

void run_genotyping(string chromosome, KmerCounter* genomic_kmer_counts, KmerCounter* read_kmer_counts, VariantReader* variant_reader, size_t kmer_abundance_peak, bool only_genotyping, bool only_phasing, Results* results) {
	// determine sets of kmers unique to each variant region
	UniqueKmerComputer kmer_computer(genomic_kmer_counts, read_kmer_counts, variant_reader, chromosome, kmer_abundance_peak);
	std::vector<UniqueKmers*> unique_kmers;
	kmer_computer.compute_unique_kmers(&unique_kmers);
	// construct HMM and run genotyping/phasing
	HMM hmm(&unique_kmers, !only_phasing, !only_genotyping);
	// store the results
	lock_guard<mutex> lock (results->m);
	results->result.insert(pair<string, vector<GenotypingResult>> (chromosome, move(hmm.get_genotyping_result())));
	// destroy unique kmers
	for (size_t i = 0; i < unique_kmers.size(); ++i) {
		delete unique_kmers[i];
		unique_kmers[i] = nullptr;
	}
}

int main (int argc, char* argv[])
{
	Timer timer;
	double time_preprocessing;
	double time_kmer_counting;
	double time_hmm;
	double time_total;

	cerr << endl;
	cerr << "program: PGGTyper (parallel) - genotyping and phasing based on kmer-counting and known haplotype sequences." << endl;
	cerr << "author: Jana Ebler" << endl << endl;
	string readfile = "";
	string reffile = "";
	string vcffile = "";
	size_t kmersize = 31;
	string outname = "result";
	string sample_name = "sample";
	size_t nr_jellyfish_threads = 1;
	size_t nr_core_threads = 1;
	bool only_genotyping = false;
	bool only_phasing = false;

	// parse the command line arguments
	CommandLineParser argument_parser;
	argument_parser.add_command("PGGTyper [options] -i <reads.fa/fq> -r <reference.fa> -v <variants.vcf>");
	argument_parser.add_mandatory_argument('i', "sequencing reads in FASTA/FASTQ format");
	argument_parser.add_mandatory_argument('r', "reference genome in FASTA format");
	argument_parser.add_mandatory_argument('v', "variants in VCF format");
	argument_parser.add_optional_argument('o', "result", "prefix of the output files");
	argument_parser.add_optional_argument('k', "31", "kmer size");
	argument_parser.add_optional_argument('s', "sample", "name of the sample (will be used in the output VCFs)");
	argument_parser.add_optional_argument('j', "1", "number of threads to use for kmer-counting");
	argument_parser.add_optional_argument('t', "1", "number of threads to use for core algorithm. Largest number of threads possible is the number of chromosomes given in the VCF.");
	argument_parser.add_flag_argument('g', "only run genotyping (Forward backward algorithm).");
	argument_parser.add_flag_argument('p', "only run phasing (Viterbi algorithm).");
	try {
		argument_parser.parse(argc, argv);
	} catch (const runtime_error& e) {
		argument_parser.usage();
		cerr << e.what() << endl;
		return 1;
	} catch (const exception& e) {
		return 0;
	}
	readfile = argument_parser.get_argument('i');
	reffile = argument_parser.get_argument('r');
	vcffile = argument_parser.get_argument('v');
	kmersize = stoi(argument_parser.get_argument('k'));
	outname = argument_parser.get_argument('o');
	sample_name = argument_parser.get_argument('s');
	nr_jellyfish_threads = stoi(argument_parser.get_argument('j'));
	nr_core_threads = stoi(argument_parser.get_argument('t'));
	only_genotyping = argument_parser.get_flag('g');
	only_phasing = argument_parser.get_flag('p');

	// print info
	cerr << "Files and parameters used:" << endl;
	argument_parser.info();

	// read allele sequences and unitigs inbetween, write them into file
	cerr << "Determine allele sequences ..." << endl;
	VariantReader variant_reader (vcffile, reffile, kmersize, sample_name);
	string segment_file = outname + "_path_segments.fasta";
	cerr << "Write path segments to file: " << segment_file << " ..." << endl;
	variant_reader.write_path_segments(segment_file);

//	// determine total genome size
//	size_t genome_kmers = variant_reader.nr_of_genomic_kmers();

	// determine chromosomes present in VCF
	vector<string> chromosomes;
	variant_reader.get_chromosomes(&chromosomes);
	cerr << "Found " << chromosomes.size() << " chromosome(s) in the VCF." << endl;

	// TODO: only for analysis
	struct rusage r_usage0;
	getrusage(RUSAGE_SELF, &r_usage0);
	cerr << "#### Memory usage until now: " << (r_usage0.ru_maxrss / 1E6) << " GB ####" << endl;

	time_preprocessing = timer.get_interval_time();

	// determine kmer copynumbers in reads
	cerr << "Count kmers in reads ..." << endl;
	KmerCounter read_kmer_counts (readfile, kmersize, nr_jellyfish_threads);
//	cerr << "Compute kmer-coverage ..." << endl;
//	size_t kmer_coverage = read_kmer_counts.computeKmerCoverage(genome_kmers);
	size_t kmer_abundance_peak = read_kmer_counts.computeHistogram(10000, outname + "_histogram.histo");
	cerr << "Computed kmer abundance peak: " << kmer_abundance_peak << endl;

	// count kmers in allele + reference sequence
	cerr << "Count kmers in genome ..." << endl;
	KmerCounter genomic_kmer_counts (segment_file, kmersize, nr_jellyfish_threads);

	// TODO: only for analysis
	struct rusage r_usage1;
	getrusage(RUSAGE_SELF, &r_usage1);
	cerr << "#### Memory usage until now: " << (r_usage1.ru_maxrss / 1E6) << " GB ####" << endl;

	// prepare output files
	if (! only_phasing) variant_reader.open_genotyping_outfile(outname + "_genotyping.vcf");
	if (! only_genotyping) variant_reader.open_phasing_outfile(outname + "_phasing.vcf");

	cerr << "Construct HMM and run core algorithm ..." << endl;
	time_kmer_counting = timer.get_interval_time();

	// determine max number of available threads (at most one thread per chromosome possible)
	size_t available_threads = min(thread::hardware_concurrency(), (unsigned int) chromosomes.size());
	if (nr_core_threads > available_threads) {
		cerr << "Warning: set nr_core_threads to " << available_threads << "." << endl;
		nr_core_threads = available_threads;
	}
	Results results;
	// create thread pool
	boost::asio::thread_pool threadPool(nr_core_threads);
	for (auto chromosome : chromosomes) {
		boost::asio::post(threadPool, boost::bind(run_genotyping, chromosome, &genomic_kmer_counts, &read_kmer_counts, &variant_reader, kmer_abundance_peak, only_genotyping, only_phasing, &results));
	} 
	threadPool.join();

	// output VCF
	cerr << "Write results to VCF ..." << endl;
	assert (results.result.size() == chromosomes.size());
	// write VCF
	for (auto it = results.result.begin(); it != results.result.end(); ++it) {
		if (!only_phasing) {
			// output genotyping results
			variant_reader.write_genotypes_of(it->first, it->second);
		}
		if (!only_genotyping) {
			// output phasing results
			variant_reader.write_phasing_of(it->first, it->second);
		}
	}

	if (! only_phasing) variant_reader.close_genotyping_outfile();
	if (! only_genotyping) variant_reader.close_phasing_outfile();

	time_hmm = timer.get_interval_time();
	time_total = timer.get_total_time();

	cerr << endl << "###### Summary ######" << endl;
	// output times
	cerr << "time spent reading input files:\t" << time_preprocessing << " sec" << endl;
	cerr << "time spent counting kmers: \t" << time_kmer_counting << " sec" << endl;
	cerr << "time spent genotyping/phasing:\t" << time_hmm << " sec" << endl;
	cerr << "total time: " << time_total  << " sec" << endl;

	// memory usage
	struct rusage r_usage;
	getrusage(RUSAGE_SELF, &r_usage);
	cerr << "Total maximum memory usage: " << (r_usage.ru_maxrss / 1E6) << " GB" << endl;

	return 0;
}
