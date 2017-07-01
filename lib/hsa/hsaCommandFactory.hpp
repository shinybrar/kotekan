#ifndef GPU_HSA_COMMAND_FACTORY_H
#define GPU_HSA_COMMAND_FACTORY_H

#include "Config.hpp"
#include "hsaDeviceInterface.hpp"
#include "bufferContainer.hpp"
#include "hsaCommand.hpp"

class hsaCommandFactory
{
public:
    hsaCommandFactory(Config &config, hsaDeviceInterface &device,
                         bufferContainer &host_buffers, const string &unique_name);
    virtual ~hsaCommandFactory();

    vector<hsaCommand *> &get_commands();
protected:
    Config &config;
    hsaDeviceInterface &device;
    bufferContainer &host_buffers;
    string unique_name;

    vector<hsaCommand *> list_commands;
};

#endif // GPU_COMMAND_FACTORY_H