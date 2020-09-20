#ifndef VG_MULTIPATH_ALIGNMENT_EMITTER_HPP_INCLUDED
#define VG_MULTIPATH_ALIGNMENT_EMITTER_HPP_INCLUDED

/**
 * \file multipath_alignment_emitter.hpp
 *
 * Defines a system for emitting multipath alignments and groups of multipath alignments in multiple formats.
 */

#include <mutex>
#include <iostream>
#include <sstream>

#include <vg/vg.pb.h>
#include <vg/io/protobuf_emitter.hpp>
#include <vg/io/stream_multiplexer.hpp>
#include "multipath_alignment.hpp"
#include "hts_alignment_emitter.hpp"
#include "alignment.hpp"

namespace vg {
using namespace std;

/*
 * Class that handles multithreaded output for multipath alignments
 */
class MultipathAlignmentEmitter : public HTSWriter {
public:
    
    /// Initialize with the intended output stream and the maximum number of threads that
    /// will be outputting.
    /// Allowed formats:
    /// - "GAMP"
    /// - "GAM", involves conversion to single path
    /// - "GAF", involves conversion to single path, requires a  graph
    /// - "SAM", "BAM", "CRAM:" requires path length map, and all input alignments must
    ///  already be surjected. If alignments have connections, requires a graph
    MultipathAlignmentEmitter(const string& filename, size_t num_threads, const string out_format = "GAMP",
                              const PathPositionHandleGraph* graph = nullptr,
                              const map<string, int64_t>* path_length = nullptr);
    ~MultipathAlignmentEmitter();
    
    /// Choose a read group to apply to all emitted alignments
    void set_read_group(const string& read_group);
    
    /// Choose a sample name to apply to all emitted alignments
    void set_sample_name(const string& sample_name);
    
    /// Emit paired read mappings as interleaved protobuf messages
    void emit_pairs(const string& name_1, const string& name_2,
                    vector<pair<multipath_alignment_t, multipath_alignment_t>>&& mp_aln_pairs,
                    vector<pair<tuple<string, bool, int64_t>, tuple<string, bool, int64_t>>>* path_positions = nullptr,
                    vector<int64_t>* tlen_limits = nullptr);
    
    /// Emit read mappings as protobuf messages
    void emit_singles(const string& name, vector<multipath_alignment_t>&& mp_alns,
                      vector<tuple<string, bool, int64_t>>* path_positions = nullptr);
    
private:
    
    enum output_format_t {GAMP, GAM, GAF, BAM, SAM, CRAM};
    output_format_t format;
    
    void convert_to_alignment(const multipath_alignment_t& mp_aln, Alignment& aln,
                              const string* prev_name = nullptr,
                              const string* next_name = nullptr) const;
    
    void convert_to_hts_unpaired(const string& name, const multipath_alignment_t& mp_aln,
                                 const string& ref_name, bool ref_rev, int64_t ref_pos,
                                 bam_hdr_t* header, vector<bam1_t*>& dest) const;
    
    void convert_to_hts_paired(const string& name_1, const string& name_2,
                               const multipath_alignment_t& mp_aln_1,
                               const multipath_alignment_t& mp_aln_2,
                               const string& ref_name_1, bool ref_rev_1, int64_t ref_pos_1,
                               const string& ref_name_2, bool ref_rev_2, int64_t ref_pos_2,
                               int64_t tlen_limit, bam_hdr_t* header, vector<bam1_t*>& dest) const;
    
    const PathPositionHandleGraph* graph;
    
    /// an Alignment emitter for each thread
    vector<unique_ptr<vg::io::ProtobufEmitter<Alignment>>> aln_emitters;
    
    /// a MultipathAlignment emitter for each thread
    vector<unique_ptr<vg::io::ProtobufEmitter<MultipathAlignment>>> mp_aln_emitters;
    
    /// read group applied to alignments
    string read_group;
    
    /// sample name applied to alignments
    string sample_name;
    
};

}


#endif
