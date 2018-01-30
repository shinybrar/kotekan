#include "fakeGpuBuffer.hpp"
#include "errors.h"
#include <time.h>
#include <sys/time.h>
#include "fpga_header_functions.h"
#include "chimeMetadata.h"

fakeGpuBuffer::fakeGpuBuffer(Config& config,
                         const string& unique_name,
                         bufferContainer &buffer_container) :
    KotekanProcess(config, unique_name, buffer_container, std::bind(&fakeGpuBuffer::main_thread, this)) {

    output_buf = get_buffer("out_buf");
    register_producer(output_buf, unique_name.c_str());

    freq = config.get_int(unique_name, "freq");
    cadence = config.get_float_default(unique_name, "cadence", 5.0);

    block_size = config.get_int(unique_name, "block_size");
    int nb1 = config.get_int(unique_name, "num_elements") / block_size;
    num_blocks = nb1 * (nb1 + 1) / 2;

    INFO("Block size %i, num blocks %i", block_size, num_blocks);
}

fakeGpuBuffer::~fakeGpuBuffer() {
}

void fakeGpuBuffer::apply_config(uint64_t fpga_seq) {
}

void fakeGpuBuffer::main_thread() {

    int frame_id = 0;
    timeval ts;

    uint64_t fpga_seq = 0;

    stream_id_t s = {0, (uint8_t)(freq % 256), 0, (uint8_t)(freq / 256)};

    while(!stop_thread) {
        int32_t * output = (int *)wait_for_empty_frame(
            output_buf, unique_name.c_str(), frame_id
        );
        if (output == NULL) break;

        // TODO adjust to allow for more than one frequency.
        // TODO remove all the 32's in here with some kind of constant/define
        INFO("Simulating GPU buffer in %s[%d]",
                output_buf->buffer_name, frame_id);

        for (int b = 0; b < num_blocks; ++b){
            for (int y = 0; y < block_size; ++y){
                for (int x = 0; x < block_size; ++x) {
                    int ind = b * block_size * block_size + x + y * block_size;
                    output[2 * ind + 0] = block_size * b; // + y;
                    output[2 * ind + 1] = block_size * b; // + x;
                    //INFO("real: %d, imag: %d", real, imag);
                }
            }
        }

        allocate_new_metadata_object(output_buf, frame_id);
        gettimeofday(&ts, NULL);

        set_fpga_seq_num(output_buf, frame_id, fpga_seq);
        set_first_packet_recv_time(output_buf, frame_id, ts);
        set_stream_id_t(output_buf, frame_id, s);

        mark_frame_full(output_buf, unique_name.c_str(), frame_id);

        fpga_seq++;

        frame_id = (frame_id + 1) % output_buf->num_frames;
        sleep(cadence);
    }
}
