#ifndef FRB_POST_PROCESS_INCOHERENT
#define FRB_POST_PROCESS_INCOHERENT

#include "KotekanProcess.hpp"
#include <vector>

using std::vector;

class frbPostProcessIncoherent : public KotekanProcess {
public:
    frbPostProcessIncoherent(Config& config_,
                  const string& unique_name,
                  bufferContainer &buffer_container);
    virtual ~frbPostProcessIncoherent();
    void main_thread();
    virtual void apply_config(uint64_t fpga_seq);

private:
    void fill_headers(unsigned char * out_buf,
                  struct FRBHeader * frb_header,
                  const uint64_t fpga_seq_num,
          const uint16_t num_L1_streams,
          uint16_t * frb_header_coarse_freq_ids,
          float * frb_header_scale,
          float * frb_header_offset);

    struct Buffer **in_buf;
    struct Buffer *frb_buf;

    //Dynamic header
    uint16_t * frb_header_beam_ids;
    uint16_t * frb_header_coarse_freq_ids;
    float * frb_header_scale;
    float * frb_header_offset;

    // Config variables
    int32_t _num_gpus;
    int32_t _samples_per_data_set;
    int32_t _nfreq_coarse;
    int32_t _downsample_time;
    int32_t _factor_upchan;
    int32_t _factor_upchan_out;
    int32_t _nbeams;
    int32_t _timesamples_per_frb_packet;
    vector<int32_t> _freq_array;

    //Derived Values
    int32_t udp_packet_size;
    int32_t udp_header_size;
    int16_t fpga_counts_per_sample;

};

#endif
