// http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html#theory

// needed for GNU extension to recvfrom: recvmmsg, bswap
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <byteswap.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>


#define PACKHEADER 114                   // Size of the packet header = PACKETSIZE-PAYLOADSIZE in bytes

#define PACKETSIZE_STOKESI  6364         // Size of the packet, including the header in bytes
#define PAYLOADSIZE_STOKESI 6250         // Size of the record = packet - header in bytes

#define PACKETSIZE_STOKESIQUV  8114      // Size of the packet, including the header in bytes
#define PAYLOADSIZE_STOKESIQUV 8000      // Size of the record = packet - header in bytes

#define PAYLOADSIZE_MAX        8000      // Maximum of payload size of I, IQUV

#define MMSG_VLEN  256            // Batch message into single syscal using recvmmsg()

#define TIMEUNIT 781250           // Conversion factor of timestamp from seconds to (1.28 us) packets
#define UMSPPACKET (1000.0)       // sleep time in microseconds between sending two packets

/*
 * Header description based on:
 * ARTS Interface Specification from BF to SC3+4
 * ASTRON_SP_066_InterfaceSpecificationSC34.pdf
 * revision 2.0
 */
typedef struct {
  unsigned char marker_byte;         // See table 3 in PDF, page 6
  unsigned char format_version;      // Version: 1
  unsigned char cb_index;            // [0,36] one compound beam per fill_ringbuffer instance:: ignore
  unsigned char tab_index;           // [0,11] all tabs per fill_ringbuffer instance
  unsigned short channel_index;      // [0,1535] all channels per fill_ringbuffer instance
  unsigned short payload_size;       // Stokes I: 6250, IQUV: 8000
  unsigned long timestamp;           // units of 1.28 us, since 1970-01-01 00:00.000 
  unsigned char sequence_number;     // SC3: Stokes I: 0-1, Stokes IQUV: 0-24
                                     // SC4: Stokes I: 0-3, Stokes IQUV: 0-49
  unsigned char reserved[7];
  unsigned long flags[3];
  unsigned char record[PAYLOADSIZE_MAX];
} packet_t;


/**
 * Print commandline optinos
 */
void printOptions() {
  printf("usage: send -c <science case> -m <science mode> -s <start packet number> -p <port>\n");
  return;
}

/**
 * Parse commandline
 */
void parseOptions(int argc, char*argv[], int *science_case, int *science_mode, unsigned long *startpacket, int *port) {
  int sets=0, setp=0, setc=0, setm=0;

  // TODO
  *startpacket = 0;
  sets = 1;

  int c;
  while((c=getopt(argc,argv,"s:p:c:m:"))!=-1) {
    switch(c) {
      // -s start packet number
      case('s'):
        *startpacket = atol(optarg);
        sets=1;
        break;

      // -p port number
      case('p'):
        *port=atoi(optarg);
        setp=1;
        break;

      // -c case
      case('c'):
        *science_case = atoi(optarg);
        setc=1;
        if (*science_case < 3 || *science_case > 4) {
          printOptions();
          exit(0);
        }
        break;

      // -m mode
      case('m'):
        *science_mode = atoi(optarg);
        setm=1;
        if (*science_mode < 0 || *science_mode > 4) {
          printOptions();
          exit(0);
        }
        break;

      default:
        fprintf(stderr, "Illegal option '%c'\n",  c);
        printOptions();
        exit(0);
    }
  }

  // All arguments are required
  if (!sets || !setp || !setc || !setm) {
    printf( "sets %i setp %i setc %i setm %i\n", sets, setp, setc, setm);
    printOptions();
    exit(EXIT_FAILURE);
  }
}

int main(int argc , char *argv[]) {
  // commandline args
  int port;
  int science_mode;        // 0: I+TAB, 1: IQUV+TAB, 2: I+IAB, 3: IQUV+IAB
  int science_case;        // 3 or 4
  unsigned long startpacket;
  parseOptions(argc, argv, &science_case, &science_mode, &startpacket, &port);

  // local variables
  int sockfd;
  struct addrinfo hints, *servinfo, *p;
  int payload_size;
  int packet_size;
  int sequence_length = 1;
  int ntabs = 1;
  int channel_delta = 1;
  unsigned char marker_field = 0;

  switch (science_case) {
    case 3:
      switch (science_mode) {
        case 0:
          // Science case 3, Stokes I + TAB
          payload_size = PAYLOADSIZE_STOKESI;
          packet_size = PACKETSIZE_STOKESI;
          sequence_length = 2;
          marker_field = 0xD0;
          ntabs = 12;
          channel_delta = 1;
          break;
        case 1:
          // Science case 3, Stokes IQUV + TAB
          payload_size = PAYLOADSIZE_STOKESIQUV;
          packet_size = PACKETSIZE_STOKESIQUV;
          sequence_length = 25;
          marker_field = 0xD1;
          ntabs = 12;
          channel_delta = 4;
          break;
        case 2:
          // Science case 3, Stokes I + IAB
          payload_size = PAYLOADSIZE_STOKESI;
          packet_size = PACKETSIZE_STOKESI;
          sequence_length = 2;
          marker_field = 0xD2;
          ntabs = 1;
          channel_delta = 1;
          break;
        case 3:
          // Science case 3, Stokes IQUV + IAB
          payload_size = PAYLOADSIZE_STOKESIQUV;
          packet_size = PACKETSIZE_STOKESIQUV;
          sequence_length = 25;
          marker_field = 0xD3;
          ntabs = 1;
          channel_delta = 4;
          break;
      }
      break;

    case 4:
      switch (science_mode) {
        case 0:
          // Science case 4, Stokes I + TAB
          payload_size = PAYLOADSIZE_STOKESI;
          packet_size = PACKETSIZE_STOKESI;
          sequence_length = 4;
          marker_field = 0xE0;
          ntabs = 12;
          channel_delta = 1;
          break;
        case 1:
          // Science case 4, Stokes IQUV + TAB
          payload_size = PAYLOADSIZE_STOKESIQUV;
          packet_size = PACKETSIZE_STOKESIQUV;
          sequence_length = 50;
          marker_field = 0xE1;
          ntabs = 12;
          channel_delta = 4;
          break;
        case 2:
          // Science case 4, Stokes I + IAB
          payload_size = PAYLOADSIZE_STOKESI;
          packet_size = PACKETSIZE_STOKESI;
          sequence_length = 4;
          marker_field = 0xE2;
          ntabs = 1;
          channel_delta = 1;
          break;
        case 3:
          // Science case 4, Stokes IQUV + IAB
          payload_size = PAYLOADSIZE_STOKESIQUV;
          packet_size = PACKETSIZE_STOKESIQUV;
          sequence_length = 50;
          marker_field = 0xE3;
          ntabs = 1;
          channel_delta = 4;
          break;
      }
      break;
  }
  printf("Sending sequence_length=%i packet_size=%i payload_size=%i marker_field=%i channel_delta=%i ntabs=%i\n",
      sequence_length, packet_size, payload_size, marker_field, channel_delta, ntabs);

  // connect to port
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  char service[256];
  snprintf(service, 255, "%i", port);

  // find possible connections
  if(getaddrinfo("127.0.0.1", service, &hints, &servinfo) != 0) {
    perror(NULL);
    exit(EXIT_FAILURE);
  }

  // loop through all the results and make a socket
  for(p = servinfo; p != NULL; p = p->ai_next) {
    sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sockfd == -1) {
      perror("talker: socket");
      continue;
    }

    if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sockfd);
      continue;
    }

    break;
  }

  if (p==NULL) {
    fprintf(stderr, "Cannot open connection\n");
    exit(EXIT_FAILURE);
  }

  // multi message setup
  packet_t packet_buffer[MMSG_VLEN];   // Buffer for batch requesting packets via recvmmsg
  unsigned int packet_idx;             // Current packet index in MMSG buffer
  struct iovec iov[MMSG_VLEN];         // IO vec structure for recvmmsg
  struct mmsghdr msgs[MMSG_VLEN];      // multimessage hearders for recvmmsg

  memset(msgs, 0, sizeof(msgs));
  for(packet_idx=0; packet_idx < MMSG_VLEN; packet_idx++) {
    iov[packet_idx].iov_base = (char *) &packet_buffer[packet_idx];
    iov[packet_idx].iov_len = packet_size;

    msgs[packet_idx].msg_hdr.msg_name    = NULL; // we don't need to know who sent the data
    msgs[packet_idx].msg_hdr.msg_iov     = &iov[packet_idx];
    msgs[packet_idx].msg_hdr.msg_iovlen  = 1;
    msgs[packet_idx].msg_hdr.msg_control = NULL; // we're not interested in OoB data
  }

  // local counters
  unsigned short curr_channel = 0;
  unsigned char curr_sequence = 0;
  unsigned char curr_tab = 0;
  unsigned long curr_time = startpacket;

  packet_t *packet;                // Pointer to current packet
  while(1) {

    // Create the next MMSB_VLEN packets
    //
    // Loop over:
    //  * tab           [0 .. 12]
    //  * sequence      [0 .. sequence_length]
    //  * channel       [0 .. 1536], in steps of channel_delta
    for(packet_idx=0; packet_idx < MMSG_VLEN; packet_idx++) {
      packet = &packet_buffer[packet_idx];

      // set constant values
      packet->marker_byte = marker_field;
      packet->format_version = 1;
      packet->cb_index = 1;
      packet->payload_size = bswap_16(payload_size);

      // update non-constant values
      packet->sequence_number = curr_sequence;
      packet->tab_index = curr_tab;
      packet->channel_index = bswap_16(curr_channel);
      packet->timestamp = bswap_64(curr_time);

      // go to next packet
      curr_channel += channel_delta;
      if (curr_channel >= 1536) {
        curr_channel = 0;
        curr_sequence++;
      }
      if (curr_sequence >= sequence_length) {
        curr_sequence = 0;
        curr_tab++;
      }
      if (curr_tab >= ntabs) {
        curr_tab = 0;
        curr_time += 800000; // 1.024 seconds per frame, in units of 1.28 microseconds
      }
    }

    // Send next batch of packets
    if (sendmmsg(sockfd, msgs, MMSG_VLEN, 0) == -1) {
      perror("ERROR Could not send packets");
      goto exit;
    }

    // slow down sending a bit
    usleep(UMSPPACKET);
  }

exit:
  // done, clean up
  free(servinfo); 
  close(sockfd);
}
