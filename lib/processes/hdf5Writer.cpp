#include "hdf5Writer.hpp"
#include "visFile.hpp"
#include "util.h"
#include "fpga_header_functions.h"
#include "chimeMetadata.h"
#include "gpuPostProcess.hpp"
#include "errors.h"
#include <time.h>
#include <unistd.h>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <fstream>

const size_t BLOCK_SIZE = 32;
const size_t MAX_NTIME = 1024;

// Functions for indexing into the buffer of data
inline uint32_t cmap(uint32_t i, uint32_t j, uint32_t n) {
    return (n * (n + 1) / 2) - ((n - i) * (n - i + 1) / 2) + (j - i);
}

inline uint32_t prod_index(uint32_t i, uint32_t j, uint32_t block, uint32_t N) {
    uint32_t b_ix = cmap(i / block, j / block, N / block);

    return block * block * b_ix + (i % block) * block + (j % block);
}

inline double tv_to_double(const timeval & tv) {
    return (tv.tv_sec + 1e-6 * tv.tv_usec);
}

// Copy the visibility triangle out of the buffer of data, allowing for a
// possible reordering of the inputs
std::vector<complex_int> copy_vis_triangle(
    const int32_t * buf, const std::vector<uint32_t>& inputmap,
    size_t block, size_t N
) {

    size_t M = inputmap.size();
    std::vector<complex_int> output(M * (M + 1) / 2);
    size_t pi = 0;
    uint32_t bi;

    if(*std::max_element(inputmap.begin(), inputmap.end()) >= N) {
        throw std::invalid_argument("Input map asks for elements out of range.");
    }

    for(auto i = inputmap.begin(); i != inputmap.end(); i++) {
        for(auto j = i; j != inputmap.end(); j++) {
            bi = prod_index(*i, *j, block, N);

            // IMPORTANT: for some reason the buffers are packed as imaginary
            // *then* real. Here we need to read out the individual components.
            output[pi].r = buf[2 * bi + 1];
            output[pi].i = buf[2 * bi];
            pi++;
        }
    }

    return output;
}

std::tuple<uint32_t, uint32_t, std::string> parse_reorder_single(json j) {
    if(!j.is_array() || j.size() != 3) {
        throw std::runtime_error("Could not parse json item for input reordering: " + j.dump());
    }

    uint32_t adc_id = j[0].get<int>();
    uint32_t chan_id = j[1].get<int>();
    std::string serial = j[2].get<std::string>();

    return std::make_tuple(adc_id, chan_id, serial);
}

std::tuple<std::vector<uint32_t>, std::vector<input_ctype>> parse_reorder(json& j) {

    uint32_t adc_id, chan_id;
    std::string serial;

    std::vector<uint32_t> adc_ids;
    std::vector<input_ctype> inputmap;

    if(!j.is_array()) {
        throw std::runtime_error("Was expecting list of input orders.");
    }

    for(auto& element : j) {
        std::tie(adc_id, chan_id, serial) = parse_reorder_single(element);

        adc_ids.push_back(adc_id);
        inputmap.emplace_back(chan_id, serial);
    }

    return std::make_tuple(adc_ids, inputmap);

}

std::tuple<std::vector<uint32_t>, std::vector<input_ctype>> default_reorder(size_t num_elements) {

    std::vector<uint32_t> adc_ids;
    std::vector<input_ctype> inputmap;

    for(uint32_t i = 0; i < num_elements; i++) {
        adc_ids.push_back(i);
        inputmap.emplace_back(i, "INVALID");
    }

    return std::make_tuple(adc_ids, inputmap);

}

hdf5Writer::hdf5Writer(Config& config,
                       const string& unique_name,
                       bufferContainer &buffer_container) :
    KotekanProcess(config, unique_name, buffer_container,
                   std::bind(&hdf5Writer::main_thread, this)) {

    // Fetch any simple configuration
    num_elements = config.get_int("/", "num_elements");
    num_freq = config.get_int(unique_name, "num_freq");
    reorder_freq = config.get_bool_default(unique_name, "reorder_frequencies",
                                           true);
    root_path = config.get_string_default(unique_name, "root_path", ".");

    // Set the list of enabled chunks (sort such that we can use
    // std::binary_search later on)
    enabled_chunks = config.get_int_array(unique_name, "enabled_chunks");
    std::sort(enabled_chunks.begin(), enabled_chunks.end());

    // Get the list of buffers that this process shoud connect to
    std::vector<std::string> buffer_names =
        config.get_string_array(unique_name, "buffers");

    // Fetch the bufferss, register on them, and store them in our buffer vector
    for(auto name : buffer_names) {
        auto buf = buffer_container.get_buffer(name);
        register_consumer(buf, unique_name.c_str());
        buffers.push_back({buf, 0});
    }

    try {
        json reorder_config = config.get_json_array(unique_name, "input_reorder");

        std::tie(input_remap, inputs) = parse_reorder(reorder_config);
    }
    catch(const std::exception& e) {
        std::tie(input_remap, inputs) = default_reorder(num_elements);
    }
    // TODO: dynamic setting of instrument name, shouldn't be hardcoded here, At
    // the moment this either uses chime, or if set to use a per_node_instrument
    // it uses the hostname of the current node
    if(config.get_bool_default(unique_name, "per_node_instrument", true)) {
        char temp[256];
        gethostname(temp, 256);
        std::string t = temp;
        // Here we trim the hostname to the first alphanumeric segment only.
        instrument_name = t.substr(0, (t + ".").find_first_of(".-"));
    } else {
        instrument_name = "chime";
    }
}

void hdf5Writer::apply_config(uint64_t fpga_seq) {

}

void hdf5Writer::main_thread() {

    uint8_t * frame = nullptr;
    struct Buffer* buf;
    unsigned int frame_id;
    size_t ntime = 0;

    // Look over the current buffers for information to setup the acquisition
    init_acq();

    while (!stop_thread) {

        // Create a new file if we need to
        if (current_file == nullptr &&
            std::binary_search(enabled_chunks.begin(),
                               enabled_chunks.end(), chunk_id)) {

            // Get the timestamp of the first buffer to label the new file
            // TODO: this won't work if we have buffers potentially out of sync
            std::tie(buf, frame_id) = buffers[0];
            double filestart = tv_to_double(get_first_packet_recv_time(buf, frame_id));

            char temp[100];
            snprintf(
                temp, sizeof(temp), "%08d_%04d.h5",
                (unsigned int)(filestart - acq_start_time), chunk_id
            );
            std::string file_name = temp;
            file_name = root_path + "/" + acq_name + "/" + file_name;
            current_file = std::unique_ptr<visFile>(
                new visFile(file_name, acq_name, instrument_name, "", freqs, inputs)
            );
        }

        // This is where all the main set of work happens. Iterate over the
        // available buffers, wait for data to appear and then attempt to write
        // the data into a file
        unsigned int buf_ind = 0;
        for(auto& buffer_pair : buffers) {
            std::tie(buf, frame_id) = buffer_pair;

            INFO("Buffer %i has frame_id=%i", buf_ind, frame_id);

            // Wait for the buffer to be filled with data
            if((frame = wait_for_full_frame(buf, unique_name.c_str(),
                                            frame_id)) == nullptr) {
                break;
            }

            uint64_t fpga_seq = get_fpga_seq_num(buf, frame_id);
            stream_id_t stream_id = get_stream_id_t(buf, frame_id);
            timeval time_v = get_first_packet_recv_time(buf, frame_id);
            uint64_t lost_samples = get_lost_timesamples(buf, frame_id);

            char time_buf[64];
            time_t temp_time = time_v.tv_sec;
            struct tm* l_time = gmtime(&temp_time);
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", l_time);

            INFO("Metadata for %s[%d]: FPGA Seq: %" PRIu64
                    ", stream ID = {create ID: %d, slot ID: %d, link ID: %d, freq ID: %d}, lost samples: %" PRIu64
                    ", time stamp: %ld.%06ld (%s.%06ld)",
                    buf->buffer_name, frame_id, fpga_seq,
                    stream_id.crate_id, stream_id.slot_id,
                    stream_id.link_id, stream_id.unused, lost_samples,
                    time_v.tv_sec, time_v.tv_usec, time_buf, time_v.tv_usec);


            // Construct the new time
            time_ctype t = {fpga_seq, tv_to_double(time_v)};

            // Lookup the frequency index if reordering, otherwise write out in buffer order
            uint32_t freq_ind = reorder_freq ? freq_stream_map[stream_id] : buf_ind;

            // Copy the visibility data into a proper triangle and write into
            // the file
            const std::vector<complex_int> vis = copy_vis_triangle(
                (int32_t *)frame, input_remap, BLOCK_SIZE, num_elements
            );

            // Create fake entries to fill out the gain and weight datasets with
            // because these don't correctly make it through kotekan yet
            std::vector<uint8_t> vis_weight(vis.size(), 255);
            std::vector<complex_int> gain_coeff(input_remap.size(), {1, 0});
            std::vector<int32_t> gain_exp(input_remap.size(), 0);

            // Add all the new information to the file.
            if(current_file != nullptr) {
                ntime = current_file->addSample(t, freq_ind, vis, vis_weight, gain_coeff, gain_exp);
            }

            // Mark the buffer as empty and move on
            mark_frame_empty(buf, unique_name.c_str(), frame_id);

            // Update the saved frame_id for this buffer
            std::get<1>(buffer_pair) = (frame_id + 1) % buf->num_frames;

            buf_ind++;
        }

        // Close the file when it gets too long
        if(ntime >= MAX_NTIME) {
            current_file = nullptr;
        }

    }

}

void hdf5Writer::init_acq() {

    struct Buffer* buf;
    unsigned int frame_id;

    // TODO: call a routine that returns a vector of all buffers that are
    // ready to read

    // Pull the required information out of each individual buffer (without
    // marking it as having been emptied)
    std::vector<stream_id_t> stream_ids;
    std::vector<timeval> start_times;

    for(auto& buffer_pair : buffers) {
        std::tie(buf, frame_id) = buffer_pair;

        wait_for_full_frame(buf, unique_name.c_str(), frame_id);
        stream_ids.push_back(get_stream_id_t(buf, frame_id));
        start_times.push_back(get_first_packet_recv_time(buf, frame_id));
    }

    // Use the per buffer into to setup the acqusition properties
    setup_freq(stream_ids);
    setup_acq_start(start_times);

    // Set the chunk_id from the set of stream IDs we are getting.
    //
    // Copy the stream_id, reset it's "unused" part, and use the bin number as
    // an id. This works because of the specfic set of IDs that are sent in the
    // current config.
    // TODO: this won't work when we've moved off the GPU nodes
    stream_id_t ts = stream_ids[0];
    ts.unused = 0;
    chunk_id = bin_number_chime(&ts);

    INFO("Running on node_id=%d", chunk_id);
}


void hdf5Writer::setup_freq(const std::vector<stream_id_t>& stream_ids) {

    // TODO: Figure out which frequencies are present from all the available data
    stream_id_t stream;
    uint32_t bin;

    // Construct the set of stream and bin ids, this pair vector is used for the
    // sort into bin order that we perform
    std::vector<std::pair<stream_id_t, uint32_t>> stream_bin_ids;

    for(auto id : stream_ids) {
        stream_bin_ids.push_back({id, bin_number_chime(&id)});
    }

    // Output all the frequencies that we have found
    std::string s;
    for(auto id : stream_bin_ids) {
        std::tie(stream, bin) = id;
        char t[32];
        snprintf(t, 32, "%i [%.2f MHz] ", bin, freq_from_bin(bin));
        s += t;
    }
    INFO("Frequency bins found: %s", s.c_str());

    // Sort the streams into bin order, this will give the order in which they
    // are written out
    if(reorder_freq) {
        std::sort(stream_bin_ids.begin(), stream_bin_ids.end(),
                  [&] (std::pair<stream_id_t, uint32_t> l,
                       std::pair<stream_id_t, uint32_t> r) {
                      return   l.second < r.second;
                  }
        );
    }
    // Fill out the frequency vector for the index map and construct the
    // std::map from stream_ids to local frequency index
    uint32_t axis_ind = 0;
    for(const auto & id : stream_bin_ids) {
        std::tie(stream, bin) = id;
        freq_stream_map[stream] = axis_ind;
        freqs.push_back({freq_from_bin(bin), (400.0 / 1024)});
        axis_ind++;
    }

}


void hdf5Writer::setup_acq_start(const std::vector<timeval>& start_times) {

    // Calculate the earliest time that a buffer was received
    timeval earliest_time = *std::min_element(
        start_times.begin(), start_times.end(),
        [] (timeval const& t1, timeval const& t2) {
            return tv_to_double(t1) < tv_to_double(t2);
        }
    );

    // Format the time (annoyingly you still have to use streams for this)
    std::ostringstream s;
    s << std::put_time(std::gmtime(&(earliest_time.tv_sec)), "%Y%m%dT%H%M%SZ");
    // Set the acq name
    acq_name = s.str() + "_" + instrument_name + "_corr";

    // Set the acq fields on the instance
    acq_start_time = tv_to_double(earliest_time);

    // Create acquisition directory. Don't bother checking if it already exists, just let it transparently fail
    std::string dir_name = root_path + "/" + acq_name;
    mkdir(dir_name.c_str(), 0755);
}


// Implemenation of ordering operator for stream id (used for map)
bool compareStream::operator()(const stream_id_t& lhs, const stream_id_t& rhs) const {
   return encode_stream_id(lhs) < encode_stream_id(rhs);
}
