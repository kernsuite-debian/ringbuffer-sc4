/**
 * Program to read from the port and write to the ringbuffer 
 * Author: Jisk Attema, based on code by Roy Smits
 *
 */
// needed for GNU extension to recvfrom: recvmmsg, bswap
#define _GNU_SOURCE

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <getopt.h>
#include <netinet/in.h>
#include <byteswap.h>
#include <math.h>
#include <signal.h>

#include "dada_hdu.h"
#include "ascii_header.h"
#include "futils.h"
#include "config.h"

#define PACKHEADER 114                   // Size of the packet header = PACKETSIZE-PAYLOADSIZE in bytes

#define PACKETSIZE_STOKESI  6364         // Size of the packet, including the header in bytes
#define PAYLOADSIZE_STOKESI 6250         // Size of the record = packet - header in bytes

#define PACKETSIZE_STOKESIQUV  8114      // Size of the packet, including the header in bytes
#define PAYLOADSIZE_STOKESIQUV 8000      // Size of the record = packet - header in bytes
#define PAYLOADSIZE_MAX        8000      // Maximum of payload size of I, IQUV

#define TIMEUNIT 781250           // Conversion factor of timestamp from seconds to (1.28 us) packets

#define MMSG_VLEN  256            // Batch message into single syscal using recvmmsg()

/* We currently use
 *  - one compound beam per instance
 *  - one instance of fill_ringbuffer connected to
 *  - one HDU
 *
 * Send on to ringbuffer a single second of data as a three dimensional array:
 * [tab_index][channel][record] of sizes [0..11][0..1535][0..paddedsize-1] = 18432 * paddedsize for a ringbuffer page
 *
 * SC3: records per 1.024s 12500
 * SC4: records per 1.024s 25000
 */

#define NCHANNELS 1536

#define SOCKBUFSIZE 67108864      // Buffer size of socket

FILE *runlog = NULL;

char *science_modes[] = {"I+TAB", "IQUV+TAB", "I+IAB", "IQUV+IAB"};

// Due to issues with the FPGAs upstream from us, the packet headers are wrong.
// Work around it for now by using this table with correct frequencies. (search for FREQISSUE below)
extern const int remap_frequency_sc4[1536];

// global state needed for SIGTERM shutdown
dada_hdu_t *signal_hdu = NULL;
size_t signal_required_size = 0;
int signal_sockfd = -1;

/*
 * Header description based on:
 * ARTS Interface Specification from BF to SC3+4
 * ASTRON_SP_066_InterfaceSpecificationSC34.pdf
 * revision 2.0
 */
typedef struct {
  unsigned char marker_byte;         // See table 3 in PDF, page 6
  unsigned char format_version;      // Version: 1
  unsigned char cb_index;            // [0,39] one compound beam per fill_ringbuffer instance:: ignore
  unsigned char tab_index;           // [0,ntabs-1] all tabs per fill_ringbuffer instance
  unsigned short channel_index;      // [0,1535] all channels per fill_ringbuffer instance
  unsigned short payload_size;       // Stokes I: 6250, IQUV: 8000
  unsigned long timestamp;           // units of 1.28 us, since 1970-01-01 00:00.000 
  unsigned char sequence_number;     // SC3: Stokes I: 0-1, Stokes IQUV: 0-24
                                     // SC4: Stokes I: 0-3, Stokes IQUV: 0-49
  unsigned char reserved[7];
  unsigned long flags[3];
  unsigned char record[PAYLOADSIZE_MAX];
} packet_t;

// #define LOG(...) {fprintf(logio, __VA_ARGS__)}; 
#define LOG(...) {fprintf(stdout, __VA_ARGS__); fprintf(runlog, __VA_ARGS__); fflush(stdout);}

/**
 * Print commandline optinos
 */
void printOptions() {
  printf("usage: fill_ringbuffer -h <header file> -k <hexadecimal key> -c <science case> -m <science mode> -s <start packet number> -d <duration (s)> -p <port> -l <logfile>\n");
  printf("e.g. fill_ringbuffer -h \"header1.txt\" -k 10 -s 11565158400000 -c 3 -m 0 -d 3600 -p 4000 -l log.txt\n");
  printf("\n\nA workaround for the incorrect frequencies in the packets headers for science case 4, stokesI, can be enabled with '-f'\n");
  return;
}

/**
 * Parse commandline
 */
void parseOptions(int argc, char*argv[], char **header, char **key, unsigned long *startpacket, float *duration, int *port, char **logfile, int *freqissue_workaround) {
  int c;

  int seth=0, setk=0, sets=0, setd=0, setp=0, setl=0;
  while((c=getopt(argc,argv,"h:k:s:d:p:l:f"))!=-1) {
    switch(c) {
      // -f work around for the FREQISSUE
      case('f'):
        *freqissue_workaround = 1;
        break;

      // -h <heaer_file>
      case('h'):
        *header = strdup(optarg);
        seth=1;
        break;

      // -k <hexadecimal_key>
      case('k'):
        *key = strdup(optarg);
        setk=1;
        break;

      // -s start packet number
      case('s'):
        *startpacket = atol(optarg);
        sets=1; 
        break;

      // -d duration in seconds
      case('d'):
        *duration=atof(optarg);
        setd=1;
        break;

      // -p port number
      case('p'):
        *port=atoi(optarg);
        setp=1;
        break;

      // -l log file
      case('l'):
        *logfile = strdup(optarg);
        setl=1;
        break;

      default:
        printOptions();
        exit(EXIT_SUCCESS);
    }
  }

  // All arguments are required
  if (!seth || !setk || !sets || !setd || !setp || !setl) {
    if (!seth) fprintf(stderr, "DADA header not set\n");
    if (!setk) fprintf(stderr, "DADA key not set\n");
    if (!sets) fprintf(stderr, "Start packet not set\n");
    if (!setd) fprintf(stderr, "Duration not set\n");
    if (!setp) fprintf(stderr, "Port not set\n");
    if (!setl) fprintf(stderr, "Log file not set\n");
    exit(EXIT_FAILURE);
  }
}

/**
 * Open a socket to read from a network port
 *
 * @param {int} port Network port to connect to
 * @returns {int} socket file descriptor
 */
int init_network(int port) {
  int sock;
  struct addrinfo hints, *servinfo, *p;
  char service[256];

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET; // set to AF_INET to force IPv4
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE; // use my IP

  snprintf(service, 255, "%i", port);
  if (getaddrinfo(NULL, service, &hints, &servinfo) != 0) {
    perror(NULL);
    exit(EXIT_FAILURE);
  }

  for(p = servinfo; p != NULL; p = p->ai_next) {
    sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sock == -1) {
      perror(NULL);
      continue;
    }

    // set socket buffer size
    int sockbufsize = SOCKBUFSIZE;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sockbufsize, (socklen_t)sizeof(int));

    if(bind(sock, p->ai_addr, p->ai_addrlen) == -1) {
      perror(NULL);
      close(sock);
      continue;
    }

    // set up, break the loop
    break;
  }

  if (p == NULL) {
    fprintf(stderr, "Cannot setup connection\n" );
    exit(EXIT_FAILURE);
  }

  free(servinfo);

  return sock;
}

/**
 * Open a connection to the ringbuffer
 * The metadata (header block) is read from file
 * The miminum_size field is updated with the actual buffer size
 *
 * @param {dada_hdu_t **} hdu pointer to a pointer of HDU
 * @param {char *} header String containing the header file name to read
 * @param {char *} key String containing the shared memeory keys as hexadecimal numbers
 * @param {size_t *} minimum_size Minimum required ring buffer page size
 * @param {int *} science_case read from the header file, and stored here
 * @param {int *} science_mode read from the header file, and stored here
 * @param {int *} padded_size read from the header file, and stored here
 * @returns {hdu *} A connected HDU
 */
dada_hdu_t *init_ringbuffer(char *header, char *key, size_t *minimum_size, int *science_case, int *science_mode, int *padded_size) {
  char *buf;
  uint64_t bufsz;
  uint64_t nbufs;
  dada_hdu_t *hdu;
  int header_incomplete = 0;

  key_t shmkey;

  multilog_t* multilog = NULL; // TODO: See if this is used in anyway by dada
  char writemode='W';     // needs to be a capital

  // create hdu
  hdu = dada_hdu_create (multilog);

  // init key
  sscanf(key, "%x", &shmkey);
  dada_hdu_set_key(hdu, shmkey);
  LOG("psrdada SHMKEY: %s\n", key);

  // connect
  if (dada_hdu_connect (hdu) < 0) {
    LOG("ERROR in dada_hdu_connect\n");
    exit(EXIT_FAILURE);
  }

  // Make data buffers readable
  if (dada_hdu_lock_write_spec (hdu, writemode) < 0) {
    LOG("ERROR in dada_hdu_lock_write_spec\n");
    exit(EXIT_FAILURE);
  }

  // get dada buffer size
  bufsz = ipcbuf_get_bufsz (hdu->header_block);

  // get write address
  buf = ipcbuf_get_next_write (hdu->header_block);
  if (! buf) {
    LOG("ERROR. Get next header block error\n");
    exit(EXIT_FAILURE);
  }

  // read header from file
  if (fileread (header, buf, bufsz) < 0) { 
    LOG("ERROR. Cannot read header from %s\n", header);
    header_incomplete = 1;
    exit(EXIT_FAILURE);
  }

  if (ascii_header_get(buf, "SCIENCE_CASE", "%i", science_case) == -1) {
    LOG("ERROR. SCIENCE_CASE not set in header\n");
    header_incomplete = 1;
  }
  if (ascii_header_get(buf, "SCIENCE_MODE", "%i", science_mode) == -1) {
    LOG("ERROR. SCIENCE_CASE not set in header\n");
    header_incomplete = 1;
  }
  if (ascii_header_get(buf, "PADDED_SIZE", "%i", padded_size) == -1) {
    LOG("ERROR. PADDED_SIZE not set in header\n");
    header_incomplete = 1;
  }

  LOG("psrdada HEADER: %s\n", header);
  if (header_incomplete) {
    exit(EXIT_FAILURE);
  }

  // tell the ringbuffer the header is filled
  if (ipcbuf_mark_filled (hdu->header_block, bufsz) < 0) {
    LOG("ERROR. Could not mark filled header block\n");
    exit(EXIT_FAILURE);
  }

  dada_hdu_db_addresses(hdu, &nbufs, &bufsz);

  if (bufsz < *minimum_size) {
    LOG("ERROR. ring buffer data block too small, should be at least %lui\n", *minimum_size);
    exit(EXIT_FAILURE);
  }

  // set the required size to the actual size
  // this is needed when marking a page full.
  // If we need to use the actual buffer size to prevent the stream from closing (too small) or reading outside of the array bounds (too big)
  *minimum_size = bufsz;

  return hdu;
}

/**
 * Try to cleanly shut down, and singal end-of-data on the ring buffer, if possible
 */
void clean_exit(int signum) {
  if (signum == SIGTERM) {
    LOG("Received SIGTERM, shutting down");
  }

  if (signal_hdu) {
    ipcbuf_enable_eod((ipcbuf_t *)signal_hdu->data_block);
    ipcbuf_mark_filled ((ipcbuf_t *)signal_hdu->data_block, signal_required_size);
  }

  // clean up and exit
  fflush(stdout);
  fflush(stderr);
  fflush(runlog);

  close(signal_sockfd);
  fclose(runlog);

  if (signum == SIGTERM) {
    exit(EXIT_SUCCESS);
  }

  exit(EXIT_FAILURE);
}

int main(int argc, char** argv) {
  // network state
  int port;                 // port number
  int sockfd = -1;          // socket file descriptor

  // ringbuffer state
  dada_hdu_t *hdu;
  char *buf; // pointer to current buffer

  // run parameters
  float duration;          // run time in seconds
  int science_case;        // 3 or 4
  int science_mode;        // 0: I+TAB, 1: IQUV+TAB, 2: I+IAB, 3: IQUV+IAB
  float missing_pct;       // Number of packets missed in percentage of expected number
  int missing;             // Number of packets missed
  unsigned long startpacket;           // Packet number to start (in units of TIMEUNIT since unix epoch)
  unsigned long endpacket;             // Packet number to stop (excluded) (in units of TIMEUNIT since unix epoch)
  int padded_size;
  int freqissue_workaround = 0; // Do we need to work around the FREQISSUE bug?

  // local vars
  char *header;
  char *key;
  char *logfile;
  const char mode = 'w';
  size_t required_size = 0;
  int ntabs = 0;
  float done_pct;
  int sequence_length; // number of packages belonging to a sequence

  packet_t packet_buffer[MMSG_VLEN];   // Buffer for batch requesting packets via recvmmsg
  unsigned int packet_idx;             // Current packet index in MMSG buffer
  struct iovec iov[MMSG_VLEN];         // IO vec structure for recvmmsg
  struct mmsghdr msgs[MMSG_VLEN];      // multimessage hearders for recvmmsg

  packet_t *packet;                 // Pointer to current packet
  unsigned char cb_index = 255;     // Current compound beam index (fixed per run)
  unsigned short curr_channel;      // Current channel index
  unsigned long curr_packet = 0;    // Current packet number (is number of packets after unix epoch)
  unsigned long sequence_time;      // Timestamp for current sequnce
  unsigned long packets_in_buffer;  // number of records processed per time segment

  // parse commandline
  if (argc == 1) {
    printOptions();
    exit(EXIT_FAILURE);
  }
  parseOptions(argc, argv, &header, &key, &startpacket, &duration, &port, &logfile, &freqissue_workaround);

  // set up logging
  if (logfile) {
    runlog = fopen(logfile, &mode);
    if (! runlog) {
      LOG("ERROR opening logfile: %s\n", logfile);
      exit(EXIT_FAILURE);
    }
    LOG("Logging to logfile: %s\n", logfile);
    free (logfile);
  }
  LOG("fill ringbuffer version: " VERSION "\n");

  // ring buffer
  LOG("Connecting to ringbuffer\n");
  hdu = init_ringbuffer(header, key, &required_size, &science_case, &science_mode, &padded_size); // sets required_size to actual size

  free(header); header = NULL;
  free(key); key = NULL;

  // calculate run length
  endpacket = startpacket + lroundf(duration * TIMEUNIT);
  LOG("Science case = %i\n", science_case);
  LOG("Science mode = %i [ %s ]\n", science_mode, science_modes[science_mode]);
  LOG("Start time (unix time) = %lu\n", startpacket / TIMEUNIT);
  LOG("End time (unix time) = %lu\n", endpacket / TIMEUNIT);
  LOG("Duration (s) = %f\n", duration);
  LOG("Start packet = %lu\n", startpacket);
  LOG("End packet = %lu\n", endpacket);

  unsigned char expected_marker_byte = 0;
  int packets_per_sample = 0;
  unsigned short expected_payload = 0;
  if (science_case == 3) {
    switch (science_mode) {
      case 0:
        expected_marker_byte = 0xD0; // I with TAB
        ntabs = 12;
        sequence_length = 2;
        packets_per_sample = ntabs * NCHANNELS * 12500 * 1 / 6250;
        expected_payload = PAYLOADSIZE_STOKESI;
        required_size = ntabs * NCHANNELS * padded_size;
        break;

      case 1:
        expected_marker_byte = 0xD1; // IQUV with TAB
        ntabs = 12;
        sequence_length = 25;
        packets_per_sample = ntabs * NCHANNELS * 12500 * 4 / 8000;
        expected_payload = PAYLOADSIZE_STOKESIQUV;
        required_size = ntabs * NCHANNELS * 12500 * 4;
        break;

      case 2:
        expected_marker_byte = 0xD2; // I with IAB
        ntabs = 1;
        sequence_length = 2;
        packets_per_sample = ntabs * NCHANNELS * 12500 * 1 / 6250;
        expected_payload = PAYLOADSIZE_STOKESI;
        required_size = ntabs * NCHANNELS * padded_size;
        break;

      case 3:
        expected_marker_byte = 0xD3; // IQUV with IAB
        ntabs = 1;
        sequence_length = 25;
        packets_per_sample = ntabs * NCHANNELS * 12500 * 4 / 8000;
        expected_payload = PAYLOADSIZE_STOKESIQUV;
        required_size = ntabs * NCHANNELS * 12500 * 4;
        break;

      default:
        fprintf(stderr, "Illegal science mode: '%i'\n", science_mode);
        exit(EXIT_FAILURE);
        break;
    }
  } else if (science_case == 4) {
    switch (science_mode) {
      case 0:
        expected_marker_byte = 0xE0; // I with TAB
        ntabs = 12;
        sequence_length = 4;
        packets_per_sample = ntabs * NCHANNELS * 25000 * 1 / 6250;
        expected_payload = PAYLOADSIZE_STOKESI;
        required_size = ntabs * NCHANNELS * padded_size;
        break;

      case 1:
        expected_marker_byte = 0xE1; // IQUV with TAB
        ntabs = 12;
        sequence_length = 50;
        packets_per_sample = ntabs * NCHANNELS * 25000 * 4 / 8000;
        expected_payload = PAYLOADSIZE_STOKESIQUV;
        required_size = ntabs * NCHANNELS * 25000 * 4;
        break;

      case 2:
        expected_marker_byte = 0xE2; // I with IAB
        ntabs = 1;
        sequence_length = 4;
        packets_per_sample = ntabs * NCHANNELS * 25000 * 1 / 6250;
        expected_payload = PAYLOADSIZE_STOKESI;
        required_size = ntabs * NCHANNELS * padded_size;
        break;

      case 3:
        expected_marker_byte = 0xE3; // IQUV with IAB
        ntabs = 1;
        sequence_length = 50;
        packets_per_sample = ntabs * NCHANNELS * 25000 * 4 / 8000;
        expected_payload = PAYLOADSIZE_STOKESIQUV;
        required_size = ntabs * NCHANNELS * 25000 * 4;
        break;

      default:
        fprintf(stderr, "Illegal science mode: '%i'\n", science_mode);
        exit(EXIT_FAILURE);
        break;
    }
  } else {
    LOG("Science case not supported");
    exit(EXIT_FAILURE);
  }

  LOG("Expected marker byte= 0x%X\n", expected_marker_byte);
  LOG("Expected payload = %i B\n", expected_payload);
  LOG("Packets per sample = %i\n", packets_per_sample);

  // sockets
  LOG("Opening network port %i\n", port);
  sockfd = init_network(port);

  // multi message setup
  memset(msgs, 0, sizeof(msgs));
  for(packet_idx=0; packet_idx < MMSG_VLEN; packet_idx++) {
    iov[packet_idx].iov_base = (char *) &packet_buffer[packet_idx];
    iov[packet_idx].iov_len = expected_payload + PACKHEADER;

    msgs[packet_idx].msg_hdr.msg_name    = NULL; // we don't need to know who sent the data
    msgs[packet_idx].msg_hdr.msg_iov     = &iov[packet_idx];
    msgs[packet_idx].msg_hdr.msg_iovlen  = 1;
    msgs[packet_idx].msg_hdr.msg_control = NULL; // we're not interested in OoB data
  }

  // clear packet counters
  packets_in_buffer = 0;

  // start at the end of the packet buffer, so the main loop starts with a recvmmsg call
  packet_idx = MMSG_VLEN - 1;
  packet = &packet_buffer[packet_idx];

  //  get a new buffer
  buf = ipcbuf_get_next_write ((ipcbuf_t *)hdu->data_block);
  packets_in_buffer = 0;
  sequence_time = curr_packet;

  // ============================================================
  // idle till start time, but keep track of which bands there are
  // ============================================================
 
  curr_packet = 0;
  packet_idx = MMSG_VLEN - 1;
  while (curr_packet < startpacket) {
    // go to next packet in the packet buffer
    packet_idx++;

    // did we reach the end of the packet buffer?
    if (packet_idx == MMSG_VLEN) {
      // read new packets from the network into the buffer
      if(recvmmsg(sockfd, msgs, MMSG_VLEN, 0, NULL) != MMSG_VLEN) {
        LOG("ERROR Could not read packets\n");
        clean_exit(0);
      }
      // go to start of buffer
      packet_idx = 0;
    }
    packet = &packet_buffer[packet_idx];

    // keep track of compound beams
    cb_index = packet->cb_index;

    // keep track of timestamps
    curr_packet = bswap_64(packet->timestamp);

    if (curr_packet != sequence_time) {
      printf( "Current packet is %li\n", curr_packet);
      sequence_time = curr_packet;
    }
  }

  // process the first (already-read) package by moving the packet_idx one back
  // this to compensate for the packet_idx++ statement in the first pass of the mainloop
  packet_idx--;

  // Try to do a clean exit on SIGTERM
  signal_hdu = hdu;
  signal_sockfd = sockfd;
  signal_required_size = required_size;
  signal(SIGTERM, clean_exit);

  LOG("STARTING WITH CB_INDEX=%i\n", cb_index);

  // ============================================================
  // run till end time
  // ============================================================

  while (1) { // loop is terminated by break statement below
    // go to next packet in the packet buffer
    packet_idx++;

    // did we reach the end of the packet buffer?
    if (packet_idx == MMSG_VLEN) {
      // read new packets from the network into the buffer
      if(recvmmsg(sockfd, msgs, MMSG_VLEN, 0, NULL) != MMSG_VLEN) {
        LOG("ERROR Could not read packets\n");
        clean_exit(0);
      }
      // go to start of buffer
      packet_idx = 0;
    }
    packet = &packet_buffer[packet_idx];

    // check marker byte
    if (packet->marker_byte != expected_marker_byte) {
      LOG("ERROR: wrong marker byte: %x instead of %x\n", packet->marker_byte, expected_marker_byte);
      clean_exit(0);
    }

    // check version
    if (packet->format_version != 1) {
      LOG("ERROR: wrong format version: %d instead of %d\n", packet->format_version, 1);
      clean_exit(0);
    }

    // check compound beam index 
    if (packet->cb_index != cb_index) {
      LOG("ERROR: unexpected compound beam index %d\n", packet->cb_index);
      clean_exit(0);
    }

    // check tab index 
    if (packet->tab_index >= ntabs) {
      LOG("ERROR: unexpected tab index %d\n", packet->tab_index);
      clean_exit(0);
    }

    // check channel
    curr_channel = bswap_16(packet->channel_index);
    if (curr_channel >= NCHANNELS) {
      LOG("ERROR: unexpected channel index %d\n", curr_channel);
      clean_exit(0);
    }

    // check payload size
    if (packet->payload_size != bswap_16(expected_payload)) {
      LOG("Warning: unexpected payload size %d\n", bswap_16(packet->payload_size));
      clean_exit(0);
    }

    // check timestamps
    curr_packet = bswap_64(packet->timestamp);
    if (curr_packet > sequence_time) {
      // start of a new time segment:
      // - check if this is the last data to process, 
      if (curr_packet >= endpacket) {
        // set End-Of-Data on the ringbuffer to have a clean shutdown of the pipeline
        ipcbuf_enable_eod((ipcbuf_t *)hdu->data_block);
      }

      //  - mark the ringbuffer as filled
      if (ipcbuf_mark_filled ((ipcbuf_t *)hdu->data_block, required_size) < 0) {
        LOG("ERROR: cannot mark buffer as filled\n");
        clean_exit(0);
      }

      // - print diagnostics
      missing = packets_per_sample - packets_in_buffer;
      missing_pct = (100.0 * missing) / (1.0 * packets_per_sample);
      done_pct = 100.0 * (1.0 * curr_packet - startpacket) / (endpacket - startpacket);
      LOG("Compound beam %4i: time %li (%6.2f%%), missing: %6.3f%% (%i)\n", cb_index, curr_packet, done_pct, missing_pct, missing);

      //  - reset the packets counter and sequence time
      packets_in_buffer = 0;
      sequence_time = curr_packet;

      // - stop when we have reached (or passed..) end packet
      if (curr_packet >= endpacket) {
        clean_exit(0);
      } else {
        //  - get a new buffer
        buf = ipcbuf_get_next_write ((ipcbuf_t *)hdu->data_block);
      }
    } else if (curr_packet < sequence_time) {
      // packet belongs to previous sequence, but we have already released that dada ringbuffer page
      continue;
    }

    // copy to ringbuffer
    if ((science_mode & 1) == 0) {
      // stokes I
      // packets contains: timeseries of PAYLOADSIZE_STOKESI elements [t0 .. tn]
      //
      // ring buffer contains matrix:
      // [ntabs][NCHANNELS][PAYLOADSIZE_STOKESI]

      if (freqissue_workaround) {
        // Work around the FREQISSUE described above
        curr_channel = remap_frequency_sc4[curr_channel];

        if (curr_channel != 9999) {
          memcpy(
            &buf[((packet->tab_index * NCHANNELS) + curr_channel) * padded_size + packet->sequence_number * PAYLOADSIZE_STOKESI],
            packet->record, PAYLOADSIZE_STOKESI);
        }
      } else {
        memcpy(
          &buf[((packet->tab_index * NCHANNELS) + curr_channel) * padded_size + packet->sequence_number * PAYLOADSIZE_STOKESI],
          packet->record, PAYLOADSIZE_STOKESI);
      }
    } else {
      // stokes IQUV
      // packets contains matrix: [t0 .. t499][c0 .. c3][the 4 components IQUV] total of 500*4*4=8000 bytes
      // t0, .., t499 = sequence_number * 500 + tx
      // c0, c1, c2, c3 = curr_channel + 0, 1, 2, 3
      //
      // ring buffer contains matrix:
      // tab             := packet->tab_index       : ranges from 0 to NTABS
      // channel_offset  := curr_channel/4          : ranges from 0 to NCHANNELS/4
      // sequence_number := packet->sequence_number : ranges from 0 to sequence_length
      //
      // [tab][channel_offset][sequence_number][PAYLOADSIZE_STOKESIQUV]
      memcpy(
        &buf[(((packet->tab_index * NCHANNELS/4) + curr_channel / 4) * sequence_length) * PAYLOADSIZE_STOKESIQUV],
        packet->record, PAYLOADSIZE_STOKESIQUV);
    }

    // book keeping
    packets_in_buffer++;
  }

  // clean up and exit
  fflush(stdout);
  fflush(stderr);
  fflush(runlog);

  close(sockfd);
  fclose(runlog);
  exit(EXIT_SUCCESS);
}
