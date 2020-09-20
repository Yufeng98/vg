/**
 * \file multipath_alignment_emitter.cpp
 *
 * Implements a system for emitting multipath alignments and groups of multipath alignments in multiple formats.
 */

#include "multipath_alignment_emitter.hpp"
#include "vg/io/json2pb.h"

using namespace vg::io;

namespace vg {
using namespace std;

MultipathAlignmentEmitter::MultipathAlignmentEmitter(const string& filename, size_t num_threads, const string out_format,
                                                     const PathPositionHandleGraph* graph, const map<string, int64_t>* path_length) :
    HTSWriter(filename,
              out_format == "SAM" || out_format == "BAM" || out_format == "CRAM" ? out_format : "SAM", // just so the assert passes
              path_length ? *path_length : map<string, int64_t>(),
              num_threads),
    graph(graph)
{
    // init the emitters for the correct output type
    if (out_format == "GAM" ) {
        format = GAM;
        aln_emitters.reserve(num_threads);
        for (int i = 0; i < num_threads; ++i) {
            aln_emitters.emplace_back(new vg::io::ProtobufEmitter<Alignment>(multiplexer.get_thread_stream(i)));
        }
    }
    else if (out_format == "GAMP") {
        format = GAMP;
        mp_aln_emitters.reserve(num_threads);
        for (int i = 0; i < num_threads; ++i) {
            mp_aln_emitters.emplace_back(new vg::io::ProtobufEmitter<MultipathAlignment>(multiplexer.get_thread_stream(i)));
        }
    }
    else if (out_format == "GAF") {
        format = GAF;
        if (graph == nullptr) {
            cerr << "error:[MultipathAlignmentEmitter] GAF format output requires a graph" << endl;
            exit(1);
        }
    }
    else if (out_format == "SAM" || out_format == "BAM" || out_format == "CRAM") {
        if (out_format == "SAM") {
            format = SAM;
        }
        else if (out_format == "BAM") {
            format = BAM;
        }
        else {
            format = CRAM;
        }
        // TODO: check for graph, in case of spliced alignments?
    }
    else {
        cerr << "error:[MultipathAlignmentEmitter] unrecognized output format " << out_format << endl;
        exit(1);
    }
}

MultipathAlignmentEmitter::~MultipathAlignmentEmitter() {
    for (auto& emitter : aln_emitters) {
        // Flush each ProtobufEmitter
        emitter->flush();
        // Make it go away before the stream
        emitter.reset();
    }
    for (auto& emitter : mp_aln_emitters) {
        // Flush each ProtobufEmitter
        emitter->flush();
        // Make it go away before the stream
        emitter.reset();
    }
}

void MultipathAlignmentEmitter::set_read_group(const string& read_group) {
    this->read_group = read_group;
}

void MultipathAlignmentEmitter::set_sample_name(const string& sample_name) {
    this->sample_name = sample_name;
}

void MultipathAlignmentEmitter::emit_pairs(const string& name_1, const string& name_2,
                                           vector<pair<multipath_alignment_t, multipath_alignment_t>>&& mp_aln_pairs,
                                           vector<pair<tuple<string, bool, int64_t>, tuple<string, bool, int64_t>>>* path_positions,
                                           vector<int64_t>* tlen_limits) {
    
    int thread_number = omp_get_thread_num();
    switch (format) {
        case GAMP:
        {
            vector<MultipathAlignment> mp_alns_out(2 * mp_aln_pairs.size());
            for (size_t i = 0; i < mp_aln_pairs.size(); ++i) {
                MultipathAlignment& mp_aln_out_1 = mp_alns_out[2 * i];
                MultipathAlignment& mp_aln_out_2 = mp_alns_out[2 * i + 1];
                to_proto_multipath_alignment(mp_aln_pairs[i].first, mp_aln_out_1);
                to_proto_multipath_alignment(mp_aln_pairs[i].second, mp_aln_out_2);
                mp_aln_out_1.set_name(name_1);
                mp_aln_out_2.set_name(name_2);
                mp_aln_out_1.set_paired_read_name(name_2);
                mp_aln_out_2.set_paired_read_name(name_1);
                if (!sample_name.empty()) {
                    mp_aln_out_1.set_sample_name(sample_name);
                    mp_aln_out_2.set_sample_name(sample_name);
                }
                if (!read_group.empty()) {
                    mp_aln_out_1.set_read_group(read_group);
                    mp_aln_out_2.set_read_group(read_group);
                }
            }
            
            mp_aln_emitters[thread_number]->write_many(std::move(mp_alns_out));
            
            if (multiplexer.want_breakpoint(thread_number)) {
                // The multiplexer wants our data.
                // Flush and create a breakpoint.
                mp_aln_emitters[thread_number]->flush();
                multiplexer.register_breakpoint(thread_number);
            }
            break;
        }
        case GAM:
        case GAF:
        {
            vector<Alignment> alns_out(2 * mp_aln_pairs.size());
            for (size_t i = 0; i < mp_aln_pairs.size(); ++i) {
                Alignment& aln_out_1 = alns_out[2 * i];
                Alignment& aln_out_2 = alns_out[2 * i + 1];
                convert_to_alignment(mp_aln_pairs[i].first, aln_out_1,
                                     nullptr,
                                     &name_2);
                convert_to_alignment(mp_aln_pairs[i].second, aln_out_2,
                                     &name_1,
                                     nullptr);
                aln_out_1.set_name(name_1);
                aln_out_2.set_name(name_2);
                if (!sample_name.empty()) {
                    aln_out_1.set_sample_name(sample_name);
                    aln_out_2.set_sample_name(sample_name);
                }
                if (!read_group.empty()) {
                    aln_out_1.set_read_group(read_group);
                    aln_out_2.set_read_group(read_group);
                }
            }
            
            if (format == GAM) {
                aln_emitters[thread_number]->write_many(std::move(alns_out));
                
                if (multiplexer.want_breakpoint(thread_number)) {
                    // The multiplexer wants our data.
                    // Flush and create a breakpoint.
                    aln_emitters[thread_number]->flush();
                }
            }
            else {
                for (auto& aln : alns_out) {
                    multiplexer.get_thread_stream(thread_number) << alignment_to_gaf(*graph, aln) << endl;
                }
            }
            multiplexer.register_breakpoint(thread_number);
            break;
        }
        case SAM:
        case BAM:
        case CRAM:
        {
            size_t thread_number = omp_get_thread_num();
            bam_hdr_t* header = ensure_header(read_group, sample_name, thread_number);
            vector<bam1_t*> records;
            records.reserve(2 * mp_aln_pairs.size());
            
            for (size_t i = 0; i < mp_aln_pairs.size(); ++i) {
                string ref_name_1, ref_name_2;
                bool ref_rev_1, ref_rev_2;
                int64_t ref_pos_1, ref_pos_2;
                tie(ref_name_1, ref_rev_1, ref_pos_1) = path_positions->at(i).first;
                tie(ref_name_2, ref_rev_2, ref_pos_2) = path_positions->at(i).second;
                int64_t tlen_limit = 0;
                if (tlen_limits) {
                    tlen_limit = tlen_limits->at(i);
                }
                convert_to_hts_paired(name_1, name_2, mp_aln_pairs[i].first, mp_aln_pairs[i].second,
                                      ref_name_1, ref_rev_1, ref_pos_1, ref_name_2, ref_rev_2, ref_pos_2,
                                      tlen_limit, header, records);
            }
            
            save_records(header, records, thread_number);
        }
            
        default:
            cerr << "error:[MultipathAlignmentEmitter] unrecognized output format" << endl;
            break;
    }
}

void MultipathAlignmentEmitter::emit_singles(const string& name, vector<multipath_alignment_t>&& mp_alns,
                                             vector<tuple<string, bool, int64_t>>* path_positions) {
    
    int thread_number = omp_get_thread_num();
    
    switch (format) {
        case GAMP:
        {
            vector<MultipathAlignment> mp_alns_out(mp_alns.size());
            for (size_t i = 0; i < mp_alns.size(); ++i) {
                MultipathAlignment& mp_aln_out = mp_alns_out[i];
                to_proto_multipath_alignment(mp_alns[i], mp_aln_out);
                mp_aln_out.set_name(name);
                if (!sample_name.empty()) {
                    mp_aln_out.set_sample_name(sample_name);
                }
                if (!read_group.empty()) {
                    mp_aln_out.set_read_group(read_group);
                }
            }
            
            mp_aln_emitters[thread_number]->write_many(std::move(mp_alns_out));
            
            if (multiplexer.want_breakpoint(thread_number)) {
                // The multiplexer wants our data.
                // Flush and create a breakpoint.
                mp_aln_emitters[thread_number]->flush();
                multiplexer.register_breakpoint(thread_number);
            }
            break;
        }
        case GAM:
        case GAF:
        {
            vector<Alignment> alns_out(mp_alns.size());
            for (size_t i = 0; i < mp_alns.size(); ++i) {
                Alignment& aln_out = alns_out[i];
                convert_to_alignment(mp_alns[i], aln_out);
                aln_out.set_name(name);
                if (!sample_name.empty()) {
                    aln_out.set_sample_name(sample_name);
                }
                if (!read_group.empty()) {
                    aln_out.set_read_group(read_group);
                }
            }
            
            if (format == GAM) {
                aln_emitters[thread_number]->write_many(std::move(alns_out));
                
                if (multiplexer.want_breakpoint(thread_number)) {
                    // The multiplexer wants our data.
                    // Flush and create a breakpoint.
                    aln_emitters[thread_number]->flush();
                }
            }
            else {
                for (auto& aln : alns_out) {
                    multiplexer.get_thread_stream(thread_number) << alignment_to_gaf(*graph, aln) << endl;
                }
            }
        }
        case SAM:
        case BAM:
        case CRAM:
        {
            size_t thread_number = omp_get_thread_num();
            bam_hdr_t* header = ensure_header(read_group, sample_name, thread_number);
            vector<bam1_t*> records;
            records.reserve(mp_alns.size());
            
            for (size_t i = 0; i < mp_alns.size(); ++i) {
                string ref_name;
                bool ref_rev;
                int64_t ref_pos;
                tie(ref_name, ref_rev, ref_pos) = path_positions->at(i);
                convert_to_hts_unpaired(name, mp_alns[i], ref_name, ref_rev, ref_pos, header, records);
            }
            
            save_records(header, records, thread_number);
        }
            
        default:
            cerr << "error:[MultipathAlignmentEmitter] unrecognized output format" << endl;
            break;
    }
}

void MultipathAlignmentEmitter::convert_to_alignment(const multipath_alignment_t& mp_aln, Alignment& aln,
                                                     const string* prev_name,
                                                     const string* next_name) const {
    optimal_alignment(mp_aln, aln);
    if (prev_name) {
        aln.mutable_fragment_prev()->set_name(*prev_name);
    }
    if (next_name) {
        aln.mutable_fragment_next()->set_name(*next_name);
    }
    // at one point vg call needed these, maybe it doesn't anymore though
    aln.set_identity(identity(aln.path()));
}

void MultipathAlignmentEmitter::convert_to_hts_unpaired(const string& name, const multipath_alignment_t& mp_aln,
                                                        const string& ref_name, bool ref_rev, int64_t ref_pos,
                                                        bam_hdr_t* header, vector<bam1_t*>& dest) const {
    
    // TODO
}

void MultipathAlignmentEmitter::convert_to_hts_paired(const string& name_1, const string& name_2,
                                                      const multipath_alignment_t& mp_aln_1,
                                                      const multipath_alignment_t& mp_aln_2,
                                                      const string& ref_name_1, bool ref_rev_1, int64_t ref_pos_1,
                                                      const string& ref_name_2, bool ref_rev_2, int64_t ref_pos_2,
                                                      int64_t tlen_limit, bam_hdr_t* header, vector<bam1_t*>& dest) const {
    // TODO
}

}
