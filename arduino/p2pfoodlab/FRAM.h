#define CMD_WREN        0x06   //0000 0110 Set Write Enable Latch
#define CMD_WRDI        0x04   //0000 0100 Write Disable
#define CMD_RDSR        0x05   //0000 0101 Read Status Register
#define CMD_WRSR        0x01   //0000 0001 Write Status Register
#define CMD_READ        0x03   //0000 0011 Read Memory Data
#define CMD_WRITE       0x02   //0000 0010 Write Memory Data

#define FRAME_COUNTER   0x1ff8
#define FIRST_FRAME     0x1ffa
#define NEXT_FRAME      0x1ffc
#define FIFO_SIZE       0x1ffe
#define META_SIZE       8
#define HEADER_SIZE     3
#define FRAM_SIZE       0x2000
#define FIFO_MAX_SIZE   0x1ff7
#define FRAM_CS         10

typedef struct frame_header_s {
  unsigned short frameSize;
  unsigned char checksum;
} frame_header_t;

