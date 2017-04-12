#ifndef HSA_PRESUM_ZERO_H
#define HSA_PRESUM_ZERO_H

#include "gpuHSACommand.hpp"

class hsaPresumZero: public gpuHSAcommand
{
public:

    hsaPresumZero(const string &kernel_name, const string &kernel_file_name,
                  gpuHSADeviceInterface &device, Config &config,
                  bufferContainer &host_buffers);

    virtual ~hsaPresumZero();

    hsa_signal_t execute(int gpu_frame_id, const uint64_t& fpga_seq,
                         hsa_signal_t precede_signal) override;

    void apply_config(const uint64_t& fpga_seq) override;

private:

    int32_t presum_len;

    void * presum_zeros;

    // TODO maybe factor these into a CHIME command object class?
    int32_t _num_local_freq;
    int32_t _num_elements;
};

#endif