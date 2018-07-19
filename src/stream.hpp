#ifndef VG_STREAM_HPP_INCLUDED
#define VG_STREAM_HPP_INCLUDED

// de/serialization of protobuf objects from/to a length-prefixed, gzipped binary stream
// from http://www.mail-archive.com/protobuf@googlegroups.com/msg03417.html

#include <cassert>
#include <iostream>
#include <istream>
#include <fstream>
#include <functional>
#include <vector>
#include <list>

#include <google/protobuf/stubs/common.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/gzip_stream.h>
#include <google/protobuf/io/coded_stream.h>

#include "blocked_gzip_output_stream.hpp"
#include "blocked_gzip_input_stream.hpp"

namespace vg {

namespace stream {

/// Protobuf will refuse to read messages longer than this size.
const size_t MAX_PROTOBUF_SIZE = 1000000000;
/// We aim to generate messages that are this size
const size_t TARGET_PROTOBUF_SIZE = MAX_PROTOBUF_SIZE/2;

/// Write objects using adaptive chunking. Takes a stream to write to, a total
/// element count to write, a guess at how many elements should be in a chunk,
/// and a function that, given a destination virtual offset in the output
/// stream (or -1), a start element, and a length, returns a Protobuf object
/// representing that range of elements.
///
/// Adaptively sets the chunk size, in elements, so that no too-large Protobuf
/// records are serialized.
///
/// Returns true on success, but throws errors on failure.
template <typename T>
bool write(std::ostream& out, size_t element_count, size_t chunk_elements,
    const std::function<T(int64_t, size_t, size_t)>& lambda) {

    // How many elements have we serialized so far
    size_t serialized = 0;
    
    BlockedGzipOutputStream bgzip_out(out);
    ::google::protobuf::io::CodedOutputStream coded_out(&bgzip_out);

    auto handle = [](bool ok) {
        if (!ok) throw std::runtime_error("stream::write: I/O error writing protobuf");
    };
    
    while (serialized < element_count) {
    
        // Work out how many elements can go in this chunk, accounting for the total element count
        chunk_elements = std::min(chunk_elements, element_count - serialized);
    
        // Work out where the chunk is going.
        // TODO: we need to back up the coded output stream after every chunk,
        // and push the partial buffer into BGZF, and get a new buffer, which 
        // wastes time.
        coded_out.Trim(); 
        int64_t virtual_offset = bgzip_out.Tell();
    
        // Serialize a chunk
        std::string chunk_data;
        handle(lambda(virtual_offset, serialized, chunk_elements).SerializeToString(&chunk_data));
    
        if (chunk_data.size() > MAX_PROTOBUF_SIZE) {
            // This is too big!
            
            if (chunk_elements > 1) {
                // But we can make it smaller. Try again at half this size.
                chunk_elements = chunk_elements / 2;
                continue;
            } else {
                // This single element is too large
                throw std::runtime_error("stream::write: message for element " +
                    std::to_string(serialized) + " too large error writing protobuf");
            }
        } else {
            // We can send this message
            
            // Say we have a group of a single message
            coded_out.WriteVarint64(1);
            handle(!coded_out.HadError());
            // and prefix each object with its size
            coded_out.WriteVarint32(chunk_data.size());
            handle(!coded_out.HadError());
            coded_out.WriteRaw(chunk_data.data(), chunk_data.size());
            handle(!coded_out.HadError());
            
            // Remember how far we've serialized now
            serialized += chunk_elements;
            
            if (chunk_data.size() < TARGET_PROTOBUF_SIZE/2) {
                // We were less than half the target size, so try being twice as
                // big next time.
                chunk_elements *= 2;
            } else if (chunk_data.size() > TARGET_PROTOBUF_SIZE && chunk_elements > 1) {
                // We were larger than the target size and we can be smaller
                chunk_elements /= 2;
            }
        }
    }
    
    return true;

}

/// Write objects using adaptive chunking. Takes a stream to write to, a total
/// element count to write, a guess at how many elements should be in a chunk,
/// and a function that, given a start element and a length, returns a Protobuf
/// object representing that range of elements.
///
/// Adaptively sets the chunk size, in elements, so that no too-large Protobuf
/// records are serialized.
///
/// Returns true on success, but throws errors on failure.
template <typename T>
bool write(std::ostream& out, size_t element_count, size_t chunk_elements,
    const std::function<T(size_t, size_t)>& lambda) {
    
    return write(out, element_count, chunk_elements,
        static_cast<const typename std::function<T(int64_t, size_t, size_t)>&>(
        [&lambda](int64_t virtual_offset, size_t chunk_start, size_t chunk_length) -> T {
        
        // Ignore the virtual offset
        return lambda(chunk_start, chunk_length);
    }));
}

/// Write objects. count should be equal to the number of objects to write.
/// count is written before the objects, but if it is 0, it is not written. To
/// get the objects, calls lambda with the highest virtual offset that can be
/// seek'd to in order to read the object (or -1 if the stream is not
/// tellable), and the index of the object to retrieve. If not all objects are
/// written, return false, otherwise true.
template <typename T>
bool write(std::ostream& out, size_t count, const std::function<T(int64_t, size_t)>& lambda) {

    // Make all our streams on the stack, in case of error.
    BlockedGzipOutputStream bgzip_out(out);
    ::google::protobuf::io::CodedOutputStream coded_out(&bgzip_out);

    auto handle = [](bool ok) {
        if (!ok) {
            throw std::runtime_error("stream::write: I/O error writing protobuf");
        }
    };
    
    // We can't seek directly to individual messages, because we can only read
    // count-prefixed groups. So the highest seek offset is going to be where
    // we are now, where the group count is being written.
    coded_out.Trim();
    int64_t virtual_offset = bgzip_out.Tell();

    // prefix the chunk with the number of objects, if any objects are to be written
    if(count > 0) {
        coded_out.WriteVarint64(count);
        handle(!coded_out.HadError());
    }

    std::string s;
    size_t written = 0;
    for (size_t n = 0; n < count; ++n, ++written) {
        handle(lambda(virtual_offset, n).SerializeToString(&s));
        if (s.size() > MAX_PROTOBUF_SIZE) {
            throw std::runtime_error("stream::write: message too large error writing protobuf");
        }
        // and prefix each object with its size
        coded_out.WriteVarint32(s.size());
        handle(!coded_out.HadError());
        coded_out.WriteRaw(s.data(), s.size());
        handle(!coded_out.HadError());
    }

    return !count || written == count;
}

/// Write objects. count should be equal to the number of objects to write.
/// count is written before the objects, but if it is 0, it is not written. To
/// get the objects, calls lambda with the index of the object to retrieve. If
/// not all objects are written, return false, otherwise true.
template <typename T>
bool write(std::ostream& out, size_t count, const std::function<T(size_t)>& lambda) {
    return write(out, count,
        static_cast<const typename std::function<T(int64_t, size_t)>&>(
        [&lambda](int64_t virtual_offset, size_t object_number) -> T {
        // Discard the virtual offset
        return lambda(object_number);
    }));
}

template <typename T>
bool write_buffered(std::ostream& out, std::vector<T>& buffer, size_t buffer_limit) {
    bool wrote = false;
    if (buffer.size() >= buffer_limit) {
        std::function<T(size_t)> lambda = [&buffer](size_t n) { return buffer.at(n); };
#pragma omp critical (stream_out)
        wrote = write(out, buffer.size(), lambda);
        buffer.clear();
    }
    return wrote;
}

/// Deserialize the input stream into the objects. Skips over groups of objects
/// with count 0. Takes a callback function to be called on the objects, with
/// the object and the blocked gzip virtual offset of its group (or -1 if the
/// input is not blocked gzipped), and another to be called per object group
/// with the group size.
template <typename T>
void for_each_with_group_length(std::istream& in,
              const std::function<void(int64_t, T&)>& lambda,
              const std::function<void(size_t)>& handle_count) {

    BlockedGzipInputStream bgzip_in(in);

    // Have a function to complain if any protobuf things report failure
    auto handle = [](bool ok) {
        if (!ok) {
            throw std::runtime_error("[stream::for_each] obsolete, invalid, or corrupt protobuf input");
        }
    };

    while (true) {
        // For each count-prefixed group
    
        // Get the offset we're at, or -1 if we can't seek/tell
        int64_t virtual_offset = bgzip_in.Tell();
        
        // Read the count
        size_t count;
        {
            ::google::protobuf::io::CodedInputStream coded_in(&bgzip_in);
            bool saw_count = coded_in.ReadVarint64((::google::protobuf::uint64*) &count);
            if (!saw_count) {
                // EOF (probably)
                return;
            }
        }
        
        // Call the count callback
        handle_count(count);
        
        std::string s;
        for (size_t i = 0; i < count; ++i) {
            uint32_t msgSize = 0;
            
            // Make sure to use a new CodedInputStream every time, because each
            // one limits all input size on the assumption it is reading a
            // single message.
            ::google::protobuf::io::CodedInputStream coded_in(&bgzip_in);
            
            // Alot space for size, and for reading next chunk's length
            coded_in.SetTotalBytesLimit(MAX_PROTOBUF_SIZE * 2, MAX_PROTOBUF_SIZE * 2);
            
            // the messages are prefixed by their size. Insist on reading it.
            handle(coded_in.ReadVarint32(&msgSize));
            
            if (msgSize > MAX_PROTOBUF_SIZE) {
                throw std::runtime_error("[stream::for_each] protobuf message of " +
                    std::to_string(msgSize) + " bytes is too long");
            }
            
            if (msgSize) {
                // If the message is nonempty, read it into a string
                handle(coded_in.ReadString(&s, msgSize));
                // Deserialize it
                T object;
                handle(object.ParseFromString(s));
                // Process it, passing along the virtual offset of the group, if available
                lambda(virtual_offset, object);
            }
        }
    }
}
    
template <typename T>
void for_each(std::istream& in,
              const std::function<void(int64_t, T&)>& lambda) {
    std::function<void(size_t)> noop = [](size_t) { };
    for_each_with_group_length(in, lambda, noop);
}

template <typename T>
void for_each(std::istream& in,
              const std::function<void(T&)>& lambda) {
    for_each(in, static_cast<const typename std::function<void(int64_t, T&)>&>([&lambda](int64_t virtual_offset, T& item) {
        lambda(item);
    }));
}

// Parallelized versions of for_each

// First, an internal implementation underlying several variants below.
// lambda2 is invoked on interleaved pairs of elements from the stream. The
// elements of each pair are in order, but the overall order in which lambda2
// is invoked on pairs is undefined (concurrent). lambda1 is invoked on an odd
// last element of the stream, if any.
template <typename T>
void for_each_parallel_impl(std::istream& in,
                            const std::function<void(T&,T&)>& lambda2,
                            const std::function<void(T&)>& lambda1,
                            const std::function<void(size_t)>& handle_count,
                            const std::function<bool(void)>& single_threaded_until_true) {

    // objects will be handed off to worker threads in batches of this many
    const size_t batch_size = 256;
    static_assert(batch_size % 2 == 0, "stream::for_each_parallel::batch_size must be even");
    // max # of such batches to be holding in memory
    size_t max_batches_outstanding = 256;
    // max # we will ever increase the batch buffer to
    const size_t max_max_batches_outstanding = 1 << 13; // 8192
    // number of batches currently being processed
    size_t batches_outstanding = 0;

    // this loop handles a chunked file with many pieces
    // such as we might write in a multithreaded process
    #pragma omp parallel default(none) shared(in, lambda1, lambda2, handle_count, batches_outstanding, max_batches_outstanding, single_threaded_until_true)
    #pragma omp single
    {
        auto handle = [](bool retval) -> void {
            if (!retval) throw std::runtime_error("obsolete, invalid, or corrupt protobuf input");
        };

        BlockedGzipInputStream bgzip_in(in);
        ::google::protobuf::io::CodedInputStream coded_in(&bgzip_in);

        std::vector<std::string> *batch = nullptr;
        
        // process chunks prefixed by message count
        size_t count;
        while (coded_in.ReadVarint64((::google::protobuf::uint64*) &count)) {
            handle_count(count);
            for (size_t i = 0; i < count; ++i) {
                if (!batch) {
                     batch = new std::vector<std::string>();
                     batch->reserve(batch_size);
                }
                
                // Reconstruct the CodedInputStream in place to reset its maximum-
                // bytes-ever-read counter, because it thinks it's reading a single
                // message.
                coded_in.~CodedInputStream();
                new (&coded_in) ::google::protobuf::io::CodedInputStream(&bgzip_in);
                // Allot space for size, and for reading next chunk's length
                coded_in.SetTotalBytesLimit(MAX_PROTOBUF_SIZE * 2, MAX_PROTOBUF_SIZE * 2);
                
                uint32_t msgSize = 0;
                // the messages are prefixed by their size
                handle(coded_in.ReadVarint32(&msgSize));
                
                if (msgSize > MAX_PROTOBUF_SIZE) {
                    throw std::runtime_error("[stream::for_each] protobuf message of " +
                        std::to_string(msgSize) + " bytes is too long");
                }
                
                if (msgSize) {
                    // pick off the message (serialized protobuf object)
                    std::string s;
                    handle(coded_in.ReadString(&s, msgSize));
                    batch->push_back(std::move(s));
                }

                if (batch->size() == batch_size) {
                    // time to enqueue this batch for processing. first, block if
                    // we've hit max_batches_outstanding.
                    size_t b;
#pragma omp atomic capture
                    b = ++batches_outstanding;
                    
                    bool do_single_threaded = !single_threaded_until_true();
                    if (b >= max_batches_outstanding || do_single_threaded) {
                        
                        // process this batch in the current thread
                        {
                            T obj1, obj2;
                            for (int i = 0; i<batch_size; i+=2) {
                                // parse protobuf objects and invoke lambda on the pair
                                handle(obj1.ParseFromString(batch->at(i)));
                                handle(obj2.ParseFromString(batch->at(i+1)));
                                lambda2(obj1,obj2);
                            }
                        } // scope obj1 & obj2
                        delete batch;
#pragma omp atomic capture
                        b = --batches_outstanding;
                        
                        if (4 * b / 3 < max_batches_outstanding
                            && max_batches_outstanding < max_max_batches_outstanding
                            && !do_single_threaded) {
                            // we went through at least 1/4 of the batch buffer while we were doing this thread's batch
                            // this looks risky, since we want the batch buffer to stay populated the entire time we're
                            // occupying this thread on compute, so let's increase the batch buffer size
                            // (skip this adjustment if you're in single-threaded mode and thus expect the buffer to be
                            // empty)
                            max_batches_outstanding *= 2;
                        }
                    }
                    else {
                        // spawn a task in another thread to process this batch
#pragma omp task default(none) firstprivate(batch) shared(batches_outstanding, lambda2, handle, single_threaded_until_true)
                        {
                            {
                                T obj1, obj2;
                                for (int i = 0; i<batch_size; i+=2) {
                                    // parse protobuf objects and invoke lambda on the pair
                                    handle(obj1.ParseFromString(batch->at(i)));
                                    handle(obj2.ParseFromString(batch->at(i+1)));
                                    lambda2(obj1,obj2);
                                }
                            } // scope obj1 & obj2
                            delete batch;
#pragma omp atomic update
                            batches_outstanding--;
                        }
                    }

                    batch = nullptr;
                }
            }
        }

        #pragma omp taskwait
        // process final batch
        if (batch) {
            {
                T obj1, obj2;
                int i = 0;
                for (; i < batch->size()-1; i+=2) {
                    handle(obj1.ParseFromString(batch->at(i)));
                    handle(obj2.ParseFromString(batch->at(i+1)));
                    lambda2(obj1, obj2);
                }
                if (i == batch->size()-1) { // odd last object
                    handle(obj1.ParseFromString(batch->at(i)));
                    lambda1(obj1);
                }
            } // scope obj1 & obj2
            delete batch;
        }
    }
}

// parallel iteration over interleaved pairs of elements; error out if there's an odd number of elements
template <typename T>
void for_each_interleaved_pair_parallel(std::istream& in,
                                        const std::function<void(T&,T&)>& lambda2) {
    std::function<void(T&)> err1 = [](T&){
        throw std::runtime_error("stream::for_each_interleaved_pair_parallel: expected input stream of interleaved pairs, but it had odd number of elements");
    };
    std::function<void(size_t)> no_count = [](size_t i) {};
    std::function<bool(void)> no_wait = [](void) {return true;};
    for_each_parallel_impl(in, lambda2, err1, no_count, no_wait);
}
    
template <typename T>
void for_each_interleaved_pair_parallel_after_wait(std::istream& in,
                                                   const std::function<void(T&,T&)>& lambda2,
                                                   const std::function<bool(void)>& single_threaded_until_true) {
    std::function<void(T&)> err1 = [](T&){
        throw std::runtime_error("stream::for_each_interleaved_pair_parallel: expected input stream of interleaved pairs, but it had odd number of elements");
    };
    std::function<void(size_t)> no_count = [](size_t i) {};
    for_each_parallel_impl(in, lambda2, err1, no_count, single_threaded_until_true);
}

// parallelized for each individual element
template <typename T>
void for_each_parallel(std::istream& in,
                       const std::function<void(T&)>& lambda1,
                       const std::function<void(size_t)>& handle_count) {
    std::function<void(T&,T&)> lambda2 = [&lambda1](T& o1, T& o2) { lambda1(o1); lambda1(o2); };
    std::function<bool(void)> no_wait = [](void) {return true;};
    for_each_parallel_impl(in, lambda2, lambda1, handle_count, no_wait);
}

template <typename T>
void for_each_parallel(std::istream& in,
              const std::function<void(T&)>& lambda) {
    std::function<void(size_t)> noop = [](size_t) { };
    for_each_parallel(in, lambda, noop);
}

    
/*
 * Refactored stream::for_each function that follows the unidirectional iterator interface.
 * Also supports seeking and telling at the group level in bgzip files.
 */
template <typename T>
class ProtobufIterator {
public:
    /// Constructor
    ProtobufIterator(std::istream& in) :
        chunk_count(0),
        chunk_idx(0),
        chunk(-1),
        bgzip_in(in)
    {
        get_next();
    }
    
    
    
//    inline ProtobufIterator<T>& operator=(const ProtobufIterator<T>& other) {
//        value = other.value;
//        chunk_count = other.chunk_count;
//        chunk_idx = other.chunk_idx;
//        chunk = other.chunk;
//        bgzip_in = other.bgzip_in;
//    }

    
    inline bool has_next() {
        return chunk != -1;
    }
    
    void get_next() {
        // Determine exactly where we are positioned, if possible, before creating the CodedInputStream
        auto virtual_offset = bgzip_in.Tell();
    
        // We need a fresh CodedInputStream every time, because of the total byte limit
        ::google::protobuf::io::CodedInputStream coded_in(&bgzip_in);
        // Alot space for size, and for reading chunk's length
        coded_in.SetTotalBytesLimit(MAX_PROTOBUF_SIZE * 2, MAX_PROTOBUF_SIZE * 2);
    
        if (chunk_count == chunk_idx) {
            // We have made it to the end of the chunk we are reading. We will start a new chunk now.
            
            if (virtual_offset == -1) {
                // We don't have seek capability, so we just count up the chunks we read.
                // On construction this is -1; bump it up to 0 for the first chunk.
                chunk++;
            } else {
                // We can seek. We need to know what offset we are at
                chunk = virtual_offset;
            }
            
            // Start at the start of the new chunk
            chunk_idx = 0;
            // Try and read its size
            if (!coded_in.ReadVarint64((::google::protobuf::uint64*) &chunk_count)) {
                // This is the end of the input stream, switch to state that
                // will match the end constructor
                chunk = -1;
                value = T();
                return;
            }
        }
        
        // Now we know we're in a chunk
        
        // The messages are prefixed by their size
        uint32_t msgSize = 0;
        handle(coded_in.ReadVarint32(&msgSize));
        
        if (msgSize > MAX_PROTOBUF_SIZE) {
            throw std::runtime_error("[stream::ProtobufIterator::get_next] protobuf message of " +
                                     std::to_string(msgSize) + " bytes is too long");
        }
        
        if (msgSize) {
            // We have a message. Parse it.
            value.Clear();
            std::string s;
            handle(coded_in.ReadString(&s, msgSize));
            handle(value.ParseFromString(s));
        } else {
            get_next();
        }
        
        // Move on to the next message in the chunk
        chunk_idx++;
    }
    
//    inline ProtobufIterator<T> operator++( int ) {
//        ProtobufIterator<T> temp = *this;
//        get_next();
//        return temp;
//    }
    
    inline T operator*(){
        return value;
    }
    
    /// Return the virtual offset of the group being currently read, to seek back to.
    /// You can't seek back to the current message, just to the start of the group.
    /// Returns -1 instead if the underlying file doesn't support seek/tell.
    inline int64_t tell_group() {
        if (bgzip_in.Tell() != -1) {
            // The backing file supports seek/tell (which we ascertain by attempting it).
            // Return the *chunk's* virtual offset (not the current one)
            return chunk;
        } else {
            // Chunk holds a count. But we need to say we can't seek.
            return -1;
        }
    }
    
    /// Seek to the given virtual offset and start reading the chunk that is there.
    /// The next value produced will be the first value in that chunk.
    /// Return false if seeking is unsupported or the seek fails.
    inline bool seek_group(int64_t virtual_offset) {
        if (virtual_offset < 0) {
            // That's not allowed
            return false;
        }
        
        // Try and do the seek
        bool sought = bgzip_in.Seek(virtual_offset);
        
        if (!sought) {
            // We can't seek
            return false;
        }
        
        // Get ready to read the group that's here
        chunk_count = 0;
        chunk_idx = 0;
        
        // Read it (or detect EOF)
        get_next();
        
        // It worked!
        return true;
    }
    
    /// Returns iterators that act like begin() and end() for a stream containing protobuf data
    static std::pair<ProtobufIterator<T>, ProtobufIterator<T>> range(std::istream& in) {
        return std::make_pair(ProtobufIterator<T>(in), ProtobufIterator<T>());
    }
    
private:
    
    T value;
    
    // This holds the number of messages that exist in the current chunk.
    size_t chunk_count;
    // This holds the number of messages read in the current chunk.
    size_t chunk_idx;
    // This holds the virtual offset of the current chunk's start, or the
    // number of the current chunk if seeking is not available.
    // If the iterator is the end iterator, this is -1.
    int64_t chunk;
    
    BlockedGzipInputStream bgzip_in;
    
    void handle(bool ok) {
        if (!ok) {
            throw std::runtime_error("[stream::ProtobufIterator] obsolete, invalid, or corrupt protobuf input");
        }
    }
};


}

}

#endif
