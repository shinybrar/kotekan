#include "hexDump.hpp"
#include "util.h"

hexDump::hexDump(Config& config,
                        const string& unique_name,
                        bufferContainer &buffer_container) :
    KotekanProcess(config, unique_name, buffer_container,
                   std::bind(&hexDump::main_thread, this)) {

    buf = get_buffer("buf");
    register_consumer(buf, unique_name.c_str());
    len = config.get_int(unique_name, "len");
    offset = config.get_int(unique_name, "offset");
}

hexDump::~hexDump() {
}

void hexDump::apply_config(uint64_t fpga_seq) {
}

void hexDump::main_thread() {

    int frame_id = 0;

    for (;;) {

        uint8_t * frame = wait_for_full_frame(buf, unique_name.c_str(), frame_id);
        INFO("hexDump: Got buffer %s[%d]", buf->buffer_name, frame_id);

        hex_dump(16, (void*)&frame[offset], len );

        mark_frame_empty(buf, unique_name.c_str(), frame_id);
        frame_id = (frame_id + 1) % buf->num_frames;
    }
}