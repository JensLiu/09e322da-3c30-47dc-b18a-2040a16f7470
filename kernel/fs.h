// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO  1   // root i-number
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

#define FSMAGIC 0x10203040

#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))                // max data block that can be refered to by a singly indirect block
#define NDBLINDIRECT (NINDIRECT * NINDIRECT)            // max data block that can be refered to by a doubly indirect block
#define MAXFILE (NDIRECT + NINDIRECT + NDBLINDIRECT)
#define DBLBASEIDX (NDIRECT + NINDIRECT)                 // base index for doubly indirect block (logical, counting data blocks only)
#define IDX2LVL1IDX(logidx) ((logidx - DBLBASEIDX) / NINDIRECT)            // logical index to level 0 index
#define IDX2LVL0IDX(logidx) ((logidx - DBLBASEIDX) % NINDIRECT)            // logical index to level 1 index
#define DBLIDX2IDX(idx1, idx0) (DBLINDBASEIDX + idx1 * NINDIRECT + idx0)   // level 1 and 0 index of doubly indirect block to logical block index

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT+2];   // Data block addresses
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

