#define _GNU_SOURCE

#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#include <memory.h>
#include <pthread.h>
#include <sched.h>
#include <getopt.h>

#include "network.h"
#include "buffers.h"
#include "file_write.h"
#include "output_power.h"

// TODO Replace defines with command line options.

// The number of buffers to keep for each disk.
#define BUFFER_DEPTH 10

int cp(const char *to, const char *from)
{
    int fd_to, fd_from;
    char buf[4096];
    ssize_t nread;
    int saved_errno;

    fd_from = open(from, O_RDONLY);
    if (fd_from < 0)
        return -1;

    fd_to = open(to, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd_to < 0)
        goto out_error;

    while (nread = read(fd_from, buf, sizeof buf), nread > 0)
    {
        char *out_ptr = buf;
        ssize_t nwritten;

        do {
            nwritten = write(fd_to, out_ptr, nread);

            if (nwritten >= 0)
            {
                nread -= nwritten;
                out_ptr += nwritten;
            }
            else if (errno != EINTR)
            {
                goto out_error;
            }
        } while (nread > 0);
    }

    if (nread == 0)
    {
        if (close(fd_to) < 0)
        {
            fd_to = -1;
            goto out_error;
        }
        close(fd_from);

        /* Success! */
        return 0;
    }

  out_error:
    saved_errno = errno;

    close(fd_from);
    if (fd_to >= 0)
        close(fd_to);

    errno = saved_errno;
    return -1;
}

void print_help() {

    printf("Program: net_to_disk\n\n");
    printf("Records data from the network to disk.\n\n");

    printf("Required Options:\n\n");
    printf("--note -c [string]            A note about the current run.\n");
    printf("--disk-set -d [cap letter]    The disk set, i.e. A, B, etc. \n");
    printf("--data-limit -l [number]      The maximum number of GB to save.\n");

    printf("\nExtra Options:\n\n");
    printf("--symlink-dir -s [dir name]   The directory to put the symlinks into, default: none\n");
    printf("--num-disks -n [number]       The number of disks, default: 10\n");
    printf("--disk-base -b [dir name]     The base dir of the disks, default: /drives/ \n");
    printf("--disable-packet-dump -x      Don't write the packets to disk \n");
    printf("--num-freq -f [number]        The number of frequencies to record, default 1024\n");
    printf("--offset -o [number]          Offset of the frequencies to record, default 0\n ");
}

void makeDirs(char * disk_base, char * disk_set, char * data_set, char * symlink_dir, int num_disks) {

    // Make the data location.
    int err = 0;
    char dir_name[100];
    for (int i = 0; i < num_disks; ++i) {

        snprintf(dir_name, 100, "%s/%s/%d/%s", disk_base, disk_set, i, data_set);
        err = mkdir(dir_name, 0777);

        if (err != -1) {
            continue;
        }

        if (errno == EEXIST) {
            printf("The data set: %s, already exists.\nPlease delete the data set, or use another name.\n", data_set);
            printf("The current data set can be deleted with: rm -fr %s/%s/*/%s && rm -fr %s/%s\n", disk_base, disk_set, data_set, symlink_dir, data_set);
        } else {
            perror("Error creating data set directory.\n");
            printf("The directory was: %s/%s/%d/%s \n", disk_base, disk_set, i, data_set);
        }
        exit(errno);
    }

    if (symlink_dir[0] != '*') {
        // Make the symlink location.
        char symlink_path[100];

        snprintf(symlink_path, 100, "%s/%s", symlink_dir, data_set);
        err = mkdir(symlink_path, 0777);

        if (err == -1) {
            if (errno == EEXIST) {
                printf("The symlink output director: %s/%s, already exists.\n", symlink_dir, data_set);
                printf("Please delete the data set, or use another name.\n");
                printf("The current data set can be deleted with: sudo rm -fr %s/%s/*/%s && sudo rm -fr %s/%s\n", disk_base, disk_set, data_set, symlink_dir, data_set);
            } else {
                perror("Error creating symlink output director directory.\n");
                printf("The symlink output directory was: %s/%s \n", symlink_dir, data_set);
            }
            exit(errno);
        }
    }
}

// Note this could be done in the file writing threads, but I put it here so there wouldn't
// be any delays caused by writing symlinks to the system disk.
void makeSymlinks(char * disk_base, char * disk_set, char * symlink_dir, char * data_set, int num_disks, int data_limit, int buffer_size) {

    int err = 0;
    char file_name[100];
    char link_name[100];
    int disk_id;

    int num_files = (data_limit * 1024) / (buffer_size/ (1024*1024));
    printf("Number of files: %d\n", num_files);

    for (int i = 0; i < num_files; ++i) {
        disk_id = i % num_disks;

        snprintf(file_name, 100, "%s/%s/%d/%s/%07d.dat", disk_base, disk_set, disk_id, data_set, i);
        snprintf(link_name, 100, "%s/%s/%07d.dat", symlink_dir, data_set, i);
        err = symlink(file_name, link_name);
        if (err == -1) {
            perror("Error creating a symlink.");
            exit(errno);
        }
    }
}

// This function is very much a hack to make life easier, but it should be replaced with something better
void copy_gains(char * base_dir, char * data_set) {
    char src[100];  // The source gains file
    char dest[100]; // The dist for the gains file copy

    snprintf(src, 100, "/home/squirrel/ch_acq/gains.pkl");
    snprintf(dest, 100, "%s/%s/gains.pkl", base_dir, data_set);

    if (cp(dest, src) != 0) {
        fprintf(stderr, "Could not copy %s to %s\n", src, dest);
    } else {
        printf("Copied gains.pkl from %s to %s\n", src, dest);
    }
}

int main(int argc, char ** argv) {

    int opt_val = 0;

    // Default values:

    char * interface = "*";
    char * note = "*";
    char * disk_set = "*";
    char data_set[150];
    int num_disks = 10;
    int data_limit = -1;
    char * symlink_dir = "*";
    char * disk_base = "/drives";
    int num_links = 8;
    int write_packets = 1;
    int write_powers = 1;
    int num_consumers = 2;

    int num_timesamples = 32*1024;
    int header_len = 58;

    // Data format
    int num_frames = 4;
    int num_inputs = 2;
    int num_freq = 1024;
    int offset = 0;

    for (;;) {
        static struct option long_options[] = {
            {"ip-address", required_argument, 0, 'i'},
            {"disk-set", required_argument, 0, 'd'},
            {"note", required_argument, 0, 'c'},
            {"num-disks", required_argument, 0, 'n'},
            {"disk-base", required_argument, 0, 'b'},
            {"data-limit", required_argument, 0, 'l'},
            {"symlink-dir", required_argument, 0, 's'},
            {"disable-packet-dump", no_argument, 0, 'x'},
            {"num-freq", required_argument, 0, 'f'},
            {"offset", required_argument, 0, 'o'},
            {"help", no_argument, 0, 'h'},
            {0, 0, 0, 0}
        };

        int option_index = 0;

        opt_val = getopt_long (argc, argv, "hi:d:c:l:n:b:s:xf:o:",
                            long_options, &option_index);

        // End of args
        if (opt_val == -1) {
            break;
        }

        switch (opt_val) {
            case 'h':
                print_help();
                return 0;
                break;
            case 'i':
                interface = optarg;
                break;
            case 'c':
                note = optarg;
                break;
            case 'd':
                disk_set = optarg;
		break;
            case 'n':
                num_disks = atoi(optarg);
                break;
            case 'b':
                disk_base = optarg;
                break;
            case 'l':
                data_limit = atoi(optarg);
                break;
            case 's':
                symlink_dir = optarg;
                break;
            case 'x':
                write_packets = 0;
                num_consumers = 1;
                break;
            case 'f':
                num_freq = atoi(optarg);
                break;
            case 'o':
                offset = atoi(optarg);
                break;
            default:
                print_help();
                break;
        }
    }

    if (data_limit <= 0) {
        printf("--data-limit needs to be set.\nUse -h for help.\n");
        return -1;
    }

    if (note[0] == '*') {
        printf("--note needs to be set.\nUse -h for help.\n");
        return -1;
    }

    if (disk_set[0] == '*') {
        printf("--disk-set needs to be set.\n Use -h for help\n");
	return -1;
    }

/*
    if (symlink_dir[0] == '*') {
        printf("--symlink-dir needs to be set.\nUse -h for help.\n");
        return -1;
    }
*/

    int packet_len = num_frames * num_inputs * num_freq + header_len;

    // Compute the data set name.
    char data_time[64];
    time_t rawtime;
    struct tm* timeinfo;
    time(&rawtime);
    timeinfo = gmtime(&rawtime);

    strftime(data_time, sizeof(data_time), "%Y%m%dT%H%M%SZ", timeinfo);
    snprintf(data_set, sizeof(data_set), "%s_aro_raw", data_time);

    if (write_packets == 1) {
        // Make the data set directory
        makeDirs(disk_base, disk_set, data_set, symlink_dir, num_disks);

        // Copy the gains file
        copy_gains(symlink_dir, data_set);

	for (int i = 0; i < num_disks; ++i) {
	    char disk_base_dir[256];
            snprintf(disk_base_dir, sizeof(disk_base_dir), "%s/%s/%d/", disk_base, disk_set, i);
            copy_gains(disk_base_dir, data_set);	
	}

        //  ** Create settings file **
        char info_file_name[256];

        snprintf(info_file_name, sizeof(info_file_name), "settings.txt", symlink_dir, data_set);

        FILE * info_file = fopen(info_file_name, "w");

        if(!info_file) {
            printf("Error creating info file: %s\n", info_file_name);
            exit(-1);
        }

        int data_format_version = 2;

        fprintf(info_file, "format_version_number=%02d\n", data_format_version);
        fprintf(info_file, "num_freq=%d\n", num_freq);
        fprintf(info_file, "num_inputs=%d\n", num_inputs);
        fprintf(info_file, "num_frames=%d\n", num_frames);
        fprintf(info_file, "num_timesamples=%d\n", num_timesamples);
        fprintf(info_file, "header_len=%d\n", header_len);
        fprintf(info_file, "packet_len=%d\n", packet_len);
        fprintf(info_file, "offset=%d\n", offset);
        fprintf(info_file, "data_bits=%d\n", 4);
        fprintf(info_file, "stride=%d\n", 1);
        fprintf(info_file, "stream_id=n/a\n");
        fprintf(info_file, "note=\"%s\"\n", note);
        fprintf(info_file, "start_time=%s\n", data_time);
        fprintf(info_file, "num_disks=%d\n", num_disks);
        fprintf(info_file, "disk_set=%s\n", disk_set);
        fprintf(info_file, "# Warning: The start time is when the program starts it, the time recorded in the packets is more accurate\n");

        fclose(info_file);

        printf("Created meta data file: settings.txt\n");

        for (int i = 0; i < num_disks; ++i) {
            char to_file[256];
            snprintf(to_file, sizeof(to_file), "%s/%s/%d/%s/settings.txt", disk_base, disk_set, i, data_set);
            int err = cp(to_file, info_file_name);
            if (err != 0) {
		fprintf(stderr, "could not copy settings");
                exit(err);
            }
        }
        if (symlink_dir[0] != '*') {
            char to_file[256];
            snprintf(to_file, sizeof(to_file), "%s/%s/settings.txt", symlink_dir, data_set);
            int err = cp(to_file, info_file_name);
            if (err != 0) {
		fprintf(stderr, "could not copy settings to symlink dir");
                exit(err);
            }
        }

    }

    //int packet_len = num_frames * num_inputs * num_freq + header_len;
    int buffer_len = (num_timesamples / num_frames) * packet_len;

    pthread_t network_t[num_links], file_write_t[num_disks], output_power_t;
    int * ret;
    struct Buffer buf;

    createBuffer(&buf, num_disks * BUFFER_DEPTH, buffer_len, num_links, num_consumers);

    if ((write_packets == 1) && (symlink_dir[0] != '*')) {
        // Create symlinks.
        printf("Creating symlinks in %s/%s\n", symlink_dir, data_set);
        makeSymlinks(disk_base, disk_set, symlink_dir, data_set, num_disks, data_limit, buffer_len);
    }

    // Let the disks flush
    sleep(5);

    // Create the network thread.
    struct network_thread_arg network_args[num_links];
    for (int i = 0; i < num_links; ++i) {
        char * ip_address = malloc(100*sizeof(char));
        snprintf(ip_address, 100, "dna%d", i);
        network_args[i].interface = ip_address;
        network_args[i].buf = &buf;
        network_args[i].bufferDepth = BUFFER_DEPTH;
        network_args[i].numLinks = num_links;
        network_args[i].data_limit = data_limit;
        network_args[i].link_id = i;
        network_args[i].num_frames = num_frames;
        network_args[i].num_inputs = num_inputs;
        network_args[i].num_freq = num_freq;
        network_args[i].offset = offset;
        HANDLE_ERROR( pthread_create(&network_t[i], NULL, (void *) &network_thread, (void *)&network_args[i] ) );
    }
    // Create the file writing threads.
    struct file_write_thread_arg file_write_args[num_disks];

    if (write_packets == 1) {
        for (int i = 0; i < num_disks; ++i) {
            file_write_args[i].buf = &buf;
            file_write_args[i].diskID = i;
            file_write_args[i].numDisks = num_disks;
            file_write_args[i].bufferDepth = BUFFER_DEPTH;
            file_write_args[i].dataset_name = data_set;
            file_write_args[i].disk_base = disk_base;
            file_write_args[i].disk_set = disk_set;
 	    HANDLE_ERROR( pthread_create(&file_write_t[i], NULL, (void *) &file_write_thread, (void *)&file_write_args[i] ) );
        }
    }

    struct output_power_thread_arg output_arg;
    if (write_powers == 1) {
        output_arg.buf = &buf;
        output_arg.bufferDepth = BUFFER_DEPTH;
        output_arg.disk_base = disk_base;
        output_arg.dataset_name = data_set;
        output_arg.diskID = 0;
        output_arg.numDisks = num_disks;
        //if (num_freq == 1024) {
        output_arg.num_freq = num_freq;
        output_arg.offset = offset;
        //} else {
        //    output_arg.num_freq = num_freq;
        //    output_arg.offset = 0;
        //}
        output_arg.num_frames = num_frames;
        output_arg.num_inputs = num_inputs;
        output_arg.integration_samples = 512;
        output_arg.num_timesamples = num_timesamples;
        output_arg.legacy_output =  0;
        HANDLE_ERROR( pthread_create(&output_power_t, NULL, (void *)&output_power_thread, (void *)&output_arg) );
    }

    // TODO Trap signals

    // clean up threads here.
    for (int i = 0; i < num_links; ++i) {
        HANDLE_ERROR( pthread_join(network_t[i], (void **) &ret) );
    }
    if (write_packets == 1) {
        for (int i = 0; i < num_disks; ++i) {
            HANDLE_ERROR( pthread_join(file_write_t[i], (void **) &ret) );
        }
    }

    if (write_powers == 1) {
        HANDLE_ERROR( pthread_join(output_power_t, (void **) &ret) );
    }

    deleteBuffer(&buf);

    return 0;
}

