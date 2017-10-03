#include "vdifReceiverMode.hpp"
#include "buffer.h"
#include "chrxUplink.hpp"
#include "gpuPostProcess.hpp"
#include "networkOutputSim.hpp"
#include "vdifStream.hpp"
#include "util.h"
#include "testDataCheck.hpp"
#include "testDataGen.hpp"
#include "rawFileRead.hpp"
#include "rawFileWrite.hpp"
#include "pyPlotResult.hpp"
#include "simVdifData.hpp"
#include "computeDualpolPower.hpp"
#include "networkPowerStream.hpp"
#include "vdif_functions.h"
#include "streamSingleDishVDIF.hpp"
#include "chimeMetadata.h"

#include <vector>
#include <string>

using std::string;
using std::vector;

vdifReceiverMode::vdifReceiverMode(Config& config) : kotekanMode(config) {
}

vdifReceiverMode::~vdifReceiverMode() {
}

void vdifReceiverMode::initalize_processes() {
    // Config values:
    int num_total_freq = config.get_int("/", "num_freq");
    int num_elements = config.get_int("/", "num_elements");
    int buffer_depth = config.get_int("/", "buffer_depth");
    int num_fpga_links = config.get_int("/", "num_links");
    int num_disks = config.get_int("/raw_capture", "num_disks");

    int integration_length = config.get_int("/", "integration_length");
    int timesteps_in = config.get_int("/", "samples_per_data_set");
    int timesteps_out = timesteps_in / integration_length;

    // Create the shared pool of buffer info objects; used for recording information about a
    // given frame and past between buffers as needed.
    struct metadataPool *pool = create_metadata_pool(5 * num_disks * buffer_depth, sizeof(chimeMetadata));
    add_metadata_pool(pool);

    DEBUG("Creating buffers...");
    // Create buffers.

    struct Buffer *output_buffer = create_buffer(output_buffer,
                                        buffer_depth,
                                        timesteps_out * (num_total_freq + 1) * num_elements * sizeof(float),
                                        pool,
                                        "output_power_buf");
    add_buffer(output_buffer);

    processFactory process_factory(config, buffer_container);
    vector<KotekanProcess *> processes = process_factory.build_processes();

    for (auto process: processes) {
        add_process(process);
    }
}
