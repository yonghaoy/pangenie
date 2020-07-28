#include <stdexcept>
#include <sstream>
#include "uniquekmers.hpp"

using namespace std;

UniqueKmers::UniqueKmers(size_t variant_position)
	:variant_pos(variant_position),
	 current_index(0),
	 local_coverage(0.0)
{}

size_t UniqueKmers::get_variant_position() {
	return this->variant_pos;
}

void UniqueKmers::insert_empty_allele(unsigned char allele_id) {
	this->alleles[allele_id] = KmerPath(); 
}

void UniqueKmers::insert_path(size_t path_id, unsigned char allele_id) {
	this->path_to_allele[path_id] = allele_id;
}

void UniqueKmers::insert_kmer(CopyNumber cn,  vector<unsigned char>& alleles){
	size_t index = this->current_index;
	this->kmer_to_copynumber.push_back(cn);
	for (auto const& a: alleles){
		this->alleles[a].set_position(index);
	}
	current_index += 1;
}

bool UniqueKmers::kmer_on_path(size_t kmer_index, size_t path_index) const {
	// check if path_id exists
	if (this->path_to_allele.find(path_index) == this->path_to_allele.end()) {
		throw runtime_error("UniqueKmers::kmer_on_path: path_index " + to_string(path_index) + " does not exist.");
	}
	// check if kmer_index is valid and look up position
	if (kmer_index < this->current_index) {
		unsigned char allele_id = this->path_to_allele.at(path_index);
		return (this->alleles.at(allele_id).get_position(kmer_index) > 0);
	} else {
		throw runtime_error("UniqueKmers::kmer_on_path: requested kmer index: " + to_string(kmer_index) + " does not exist.");
	}
}

CopyNumberAssignment UniqueKmers::combine_paths(unsigned char allele_id1, unsigned char allele_id2) {
	return this->alleles.at(allele_id1) + this->alleles.at(allele_id2);
}

CopyNumber UniqueKmers::get_copynumber_of(size_t kmer_index) {
	if (kmer_index < this->current_index) {
		return this->kmer_to_copynumber[kmer_index];
	} else {
		throw runtime_error("UniqueKmers::get_copynumber_of: requested kmer index: " + to_string(kmer_index) + " does not exist.");
	}
}

size_t UniqueKmers::size() const {
	return this->current_index;
}

size_t UniqueKmers::get_nr_paths() const {
	return this->path_to_allele.size();
}

void UniqueKmers::get_path_ids(vector<size_t>& p, vector<unsigned char>& a, vector<size_t>* only_include) {
	if (only_include != nullptr) {
		// only return paths that are also contained in only_include
		for (auto p_it = only_include->begin(); p_it != only_include->end(); ++p_it) {
			// check if path is in path_to_allele
			auto it = this->path_to_allele.find(*p_it);
			if (it != this->path_to_allele.end()) {
				p.push_back(*p_it);
				a.push_back(it->second);
			}
		}
	} else {
		// return all paths and corresponding alleles
		for (auto it = this->path_to_allele.begin(); it != this->path_to_allele.end(); ++it) {
			p.push_back(it->first);
			a.push_back(it->second);
		}
	}
}

void UniqueKmers::get_allele_ids(vector<unsigned char>& a) {
	for (auto it = this->alleles.begin(); it != this->alleles.end(); ++it) {
		a.push_back(it->first);
	}
}

ostream& operator<< (ostream& stream, const UniqueKmers& uk) {
	stream << "UniqueKmers for variant: " << uk.variant_pos << endl;
	for (size_t i = 0; i < uk.size(); ++i) {
		CopyNumber cn = uk.kmer_to_copynumber[i];
		stream << i << ": " << cn.get_probability_of(0) << " " << cn.get_probability_of(1) <<  " " << cn.get_probability_of(2) << endl;
	}
	stream << "alleles:" << endl;
	for (auto it = uk.alleles.begin(); it != uk.alleles.end(); ++it) {
		stream << (unsigned int) it->first << "\t" << it->second.convert_to_string() << endl;
	}
	stream << "paths:" << endl;
	for (auto it = uk.path_to_allele.begin(); it != uk.path_to_allele.end(); ++it) {
		stream << it->first << " covers allele " << (unsigned int) it->second << endl;
	}
	return stream;
}

void UniqueKmers::set_coverage(double local_coverage) {
	this->local_coverage = local_coverage;
}

double UniqueKmers::get_coverage() const {
	return this->local_coverage;
}

map<unsigned char, int> UniqueKmers::kmers_on_alleles () const {
	map<unsigned char, int> result;
	for (auto it = this->alleles.begin(); it != this->alleles.end(); ++it) {
		result[it->first] = alleles.at(it->first).nr_kmers();
	}
	return result;
}
