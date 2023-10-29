#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

// I would like to use two locks for two different
// buffers, but the lab provided with only one lock
// so let us explore a solution using one lock
struct spinlock e1000_lock;

#define assert(expr) if (!expr) panic("assert failed " #expr"\n")

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //  
  acquire(&e1000_lock);
  uint32 tail_idx = regs[E1000_TDT];
  uint32 head_idx = regs[E1000_TDH];
  struct tx_desc *tail = &tx_ring[tail_idx];
  
  if (!(tail->status & E1000_TXD_STAT_DD)) {
    // overflow
    release(&e1000_lock);
    return -1;
  }

  // free possible mbufs (search 'buf\[head_idx, tail_idx)' area)
  for (int i = tail_idx; i < head_idx + TX_RING_SIZE; i++) {
    if ((tx_ring[i % TX_RING_SIZE].status & E1000_TXD_STAT_DD) && tx_mbufs[i % TX_RING_SIZE]) {
      mbuffree(tx_mbufs[i % TX_RING_SIZE]);
      tx_mbufs[i % TX_RING_SIZE] = 0;
    }
  }

  // stash pointers
  assert(tx_mbufs[tail_idx] == 0);
  tx_mbufs[tail_idx] = m;

  // update tx_desc for HW
  tail->addr = (uint64) m->head;  // NOTE: not m->buf
  tail->length = m->len;
  // EOP: end of packet
  // RS: report status (sets DD status)
  tail->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  tail->status &= ~E1000_RXD_STAT_DD;

  // update ring position -> start transmiting
  regs[E1000_TDT] = (tail_idx + 1) % TX_RING_SIZE;
  release(&e1000_lock);
  return 0;
}

static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //
  
  acquire(&e1000_lock);
  uint64 tail_idx = regs[E1000_RDT]; 
  uint64 head_idx = regs[E1000_RDH];
  int i = (tail_idx + 1) % RX_RING_SIZE; // NOTE: start not at the tail
  struct rx_desc *rp = &rx_ring[i];
  int cache_idx = 0;
  struct mbuf *buf_cache[RX_RING_SIZE];
  for (; 
      (rp->status & E1000_RXD_STAT_DD) && i < head_idx + RX_RING_SIZE; 
      rp = &rx_ring[++i % RX_RING_SIZE]) {
    
    struct mbuf *bp = rx_mbufs[i % RX_RING_SIZE];
    bp->head = (char *) rx_ring[i % RX_RING_SIZE].addr;
    bp->len = rx_ring[i % RX_RING_SIZE].length;

    // instead of calling net_rx(bp)
    // cache the buffer and then unlock
    // to avoid deadlock
    buf_cache[cache_idx++] = bp;

    bp = mbufalloc(0); 
    if (bp == 0)
      panic("e1000_rev: unable to alloc mbuf\n");
    rp->status = 0;
  }
  
  regs[E1000_RDT] = (i - 1) % RX_RING_SIZE;
  release(&e1000_lock);

  for (i = 0; i < cache_idx; i++) {
    net_rx(buf_cache[i]);
  }

}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
