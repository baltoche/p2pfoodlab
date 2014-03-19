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

/*
 * FRAM API functions
 */

// init the bus then clears the FRAM
void initFRAM();

// clears the FRAM.
void FRAMClear();

// copy one frame into (*frame) then deletes it from the FRAM.
// (*frame) must be big enough to hold the data
// returns 0 on success
//        -1 if no frames are available
//        -2 if the RAM is corupted
int FRAMReadFrame(byte *frame);

// return the header of the next available frame.
// usefull to prepare a buffer of the right size.
// returns all fields to 0 if no frames are available.
frame_header_t FRAMReadframeHeader();

// return the ammount of frames curently stored in FRAM.
unsigned short FRAMReadFrameCounter();

// Stores a buffer (*buf) of size (count) into a new frame.
// Returns 0 on success
//        -1 if FRAM is full.
int FRAMWriteFrame(byte *buf, unsigned short count);

/*
 * FRAM internal functions
 */

// Write buffer (*buf) of size (count) at address (addr) in FRAM
// returns 0 on success
//        -1 on failure (invalid address)
int FRAMWrite(int addr, byte *buf, int count);

// Write buffer (*buf) of size (count) at address (addr) in FRAM
// returns 0 on success
//        -1 on failure (invalid address)
int FRAMRead(int addr, byte *buf, int count);

// Read and warps around if at the end of the FRAM
// returns 0 on success 
//        -1 if first read fails
//        -2 if seccond read fails
//        -3 if both reads fail.
int FRAMCircularRead(unsigned short addr, byte *buf, unsigned short maxAddr, int count);

// Write and warps around if at the end of the FRAM
// returns 0 on success
//        -1 if first write fails
//        -2 if seccond write fails
//        -3 if both writes fail.
int FRAMCircularWrite(unsigned short addr, byte *buf, unsigned short maxAddr, int count);

