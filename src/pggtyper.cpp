#include <iostream>
#include <sstream>
#include <sys/resource.h>
#include "kmercounter.hpp"
#include "emissionprobabilitycomputer.hpp"
#include "copynumber.hpp"
#include "variantreader.hpp"
#include "uniquekmercomputer.hpp"
#include "hmm.hpp"
#include "commandlineparser.hpp"
#include "timer.hpp"

using namespace std;

int main (int argc, char* argv[])
{
	Timer timer;
	double time_preprocessing;
	double time_kmer_counting;
	double time_hmm;
	double time_total;

	cerr << endl;
	cerr << "program: PGGTyper - genotyping and phasing based on kmer-counting and known haplotype sequences." << endl;
	cerr << "author: Jana Ebler" << endl << endl;
	string readfile = "";
	string reffile = "";
	string vcffile = "";
	size_t kmersize = 31;
	string outname = "result";
	string sample_name = "sample";
	bool only_genotyping = false;
	bool only_phasing = false;
	size_t small_kmersize = 5;

	// parse the command line arguments
	CommandLineParser argument_parser;
	argument_parser.add_command("PGGTyper [options] -i <reads.fa/fq> -r <reference.fa> -v <variants.vcf>");
	argument_parser.add_mandatory_argument('i', "sequencing reads in FASTA/FASTQ format");
	argument_parser.add_mandatory_argument('r', "reference genome in FASTA format");
	argument_parser.add_mandatory_argument('v', "variants in VCF format");
	argument_parser.add_optional_argument('o', "result", "prefix of the output files");
	argument_parser.add_optional_argument('k', "31", "kmer size");
	argument_parser.add_optional_argument('s', "sample", "name of the sample (will be used in the output VCFs)");
	argument_parser.add_optional_argument('m', "5", "small kmer size");
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
	only_genotyping = argument_parser.get_flag('g');
	only_phasing = argument_parser.get_flag('p');
	small_kmersize = stoi(argument_parser.get_argument('m'));

	// print info
	cerr << "Files and parameters used:" << endl;
	argument_parser.info();

	// read allele sequences and unitigs inbetween, write them into file
	cerr << "Determine allele sequences ..." << endl;
	FastaReader reffile_reader(reffile);
	VariantReader variant_reader (vcffile, &reffile_reader, kmersize, sample_name);
	string segment_file = outname + "_path_segments.fasta";
	cerr << "Write path segments to file: " << segment_file << " ..." << endl;
	variant_reader.write_path_segments(segment_file, true);

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
	KmerCounter read_kmer_counts (readfile, kmersize);
//	cerr << "Compute kmer-coverage ..." << endl;
//	size_t kmer_coverage = read_kmer_counts.computeKmerCoverage(genome_kmers);
	size_t kmer_abundance_peak = read_kmer_counts.computeHistogram(10000, outname + "_histogram.histo");
	cerr << "Computed kmer abundance peak: " << kmer_abundance_peak << endl;

	// count kmers in allele + reference sequence
	cerr << "Count kmers in genome ..." << endl;
	KmerCounter genomic_kmer_counts (segment_file, kmersize);

	// correct kmer counts
	string training_file = segment_file + ".train";
	read_kmer_counts.correct_read_counts(&genomic_kmer_counts, &reffile_reader, training_file, small_kmersize, 1/100.0);

	size_t corrected_kmer_abundance_peak = read_kmer_counts.computeHistogram(10000, outname + "_corrected-histogram.histo");
	cerr << "Computed corrected kmer abundance peak: " << corrected_kmer_abundance_peak << endl;

	// TODO: only for analysis
	struct rusage r_usage1;
	getrusage(RUSAGE_SELF, &r_usage1);
	cerr << "#### Memory usage until now: " << (r_usage1.ru_maxrss / 1E6) << " GB ####" << endl;

	// prepare output files
	if (! only_phasing) variant_reader.open_genotyping_outfile(outname + "_genotyping.vcf");
	if (! only_genotyping) variant_reader.open_phasing_outfile(outname + "_phasing.vcf");

	time_kmer_counting = timer.get_interval_time();

	for (auto& chromosome : chromosomes) {
		cerr << "Processing chromosome " << chromosome << "." << endl;
		cerr << "Determine unique kmers ..." << endl;
		// determine sets of kmers unique to each variant region
		UniqueKmerComputer kmer_computer(&genomic_kmer_counts, &read_kmer_counts, &variant_reader, chromosome, kmer_abundance_peak);
		std::vector<UniqueKmers*> unique_kmers;
		kmer_computer.compute_unique_kmers(&unique_kmers);

		struct rusage r_usagei;
		getrusage(RUSAGE_SELF, &r_usagei);
		cerr << "#### Memory usage until now: " << (r_usagei.ru_maxrss / 1E6) << " GB ####" << endl;

		// get variants on this chromosome
		cerr << "Construct HMM" << endl;
		HMM hmm(&unique_kmers, !only_phasing, !only_genotyping);

		if (! only_phasing) {
			// output the genotyping results
			cerr << "Write genotyping output ..." << endl;
			variant_reader.write_genotypes_of(chromosome, hmm.get_genotyping_result());
		}

		if (! only_genotyping) {
			// output the phasing results
			cerr << "Write phasing output ..." << endl;
			variant_reader.write_phasing_of(chromosome, hmm.get_genotyping_result());
		}

		// destroy unique kmers
		for (size_t i = 0; i < unique_kmers.size(); ++i) {
			delete unique_kmers[i];
			unique_kmers[i] = nullptr;
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
