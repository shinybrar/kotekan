#include "hsaBeamformUpchan.hpp"
#include "hsaBase.h"

hsaBeamformUpchan::hsaBeamformUpchan(const string& kernel_name, const string& kernel_file_name,
                            hsaDeviceInterface& device, Config& config,
			    bufferContainer& host_buffers,
			    const string &unique_name) :
    hsaCommand(kernel_name, kernel_file_name, device, config, host_buffers, unique_name) {
    apply_config(0);


}

hsaBeamformUpchan::~hsaBeamformUpchan() {
}

void hsaBeamformUpchan::apply_config(const uint64_t& fpga_seq) {
    hsaCommand::apply_config(fpga_seq);

    _num_elements = config.get_int(unique_name, "num_elements");
    _samples_per_data_set = config.get_int(unique_name, "samples_per_data_set");
    _downsample_time = config.get_int(unique_name, "downsample_time");
    _downsample_freq = config.get_int(unique_name, "downsample_freq");

    input_frame_len = _num_elements * (_samples_per_data_set+32) * 2 * sizeof(float);
    output_frame_len = _num_elements * (_samples_per_data_set/_downsample_time/_downsample_freq/2) * sizeof(float);


}

hsa_signal_t hsaBeamformUpchan::execute(int gpu_frame_id, const uint64_t& fpga_seq, hsa_signal_t precede_signal) {

    struct __attribute__ ((aligned(16))) args_t {
        void *input_buffer;
        void *output_buffer;
    } args;
    memset(&args, 0, sizeof(args));

    args.input_buffer = device.get_gpu_memory_array("transposed_output", gpu_frame_id, input_frame_len);
    args.output_buffer = device.get_gpu_memory_array("frb_output", gpu_frame_id, output_frame_len);
    // Allocate the kernel argument buffer from the correct region.
    memcpy(kernel_args[gpu_frame_id], &args, sizeof(args));

    kernelParams params;
    params.workgroup_size_x = 64;
    params.workgroup_size_y = 1;
    params.grid_size_x = _samples_per_data_set/6;
    params.grid_size_y = 1024;
    params.num_dims = 2;

    params.private_segment_size = 0;
    params.group_segment_size = 3072;

    signals[gpu_frame_id] = enqueue_kernel(params, gpu_frame_id);

    return signals[gpu_frame_id];
}
