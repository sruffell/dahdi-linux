/*
 * WCB410P  Quad-BRI PCI Driver
 * Written by Andrew Kohlsmith <akohlsmith@mixdown.ca>
 *
 * Copyright (C) 2009 Digium, Inc.
 * All rights reserved.
 *
 */

/*
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2 as published by the
 * Free Software Foundation. See the LICENSE file included with
 * this program for more details.
 */

#include <linux/init.h>

#include <linux/kernel.h>	/* printk() */
#include <linux/errno.h>	/* error codes */
#include <linux/module.h>
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/pci.h>		/* for PCI structures */
#include <linux/delay.h>
#include <asm/io.h>
#include <linux/spinlock.h>
#include <linux/device.h>	/* dev_err() */
#include <linux/interrupt.h>
#include <asm/system.h>		/* cli(), *_flags */
#include <asm/uaccess.h>	/* copy_*_user */
#include <linux/workqueue.h>	/* work_struct */
#include <linux/timer.h>	/* timer_struct */
#include <linux/moduleparam.h>
#include <linux/proc_fs.h>

#include <dahdi/kernel.h>

#include "wcb4xxp.h"

#ifndef BIT     /* added in 2.6.24 */
#define BIT(i)          (1UL << (i))
#endif
#define BIT_SET(x, i)    ((x) |= BIT(i))
#define BIT_CLR(x, i)    ((x) &= ~BIT(i))
#define IS_SET(x, i)     (((x) & BIT(i)) != 0)
#define BITMASK(i)      (((u64)1 << (i)) - 1)


#if (DAHDI_CHUNKSIZE != 8)
#error Sorry, wcb4xxp does not support chunksize != 8
#endif

//#define SIMPLE_BCHAN_FIFO
//#define DEBUG_LOWLEVEL_REGS			/* debug __pci_in/out, not b4xxp_setreg */

#define DEBUG_GENERAL 		(1 << 0)	/* general debug messages */
#define DEBUG_DTMF 		(1 << 1)	/* emit DTMF detector messages */
#define DEBUG_REGS 		(1 << 2)	/* emit register read/write, but only if the kernel's DEBUG is defined */
#define DEBUG_FOPS  		(1 << 3)	/* emit file operation messages */
#define DEBUG_ECHOCAN 		(1 << 4)
#define DEBUG_ST_STATE		(1 << 5)	/* S/T state machine */
#define DEBUG_HDLC		(1 << 6)	/* HDLC controller */
#define DEBUG_ALARM		(1 << 7)	/* alarm changes */

#define DBG			(debug & DEBUG_GENERAL)
#define DBG_DTMF		(debug & DEBUG_DTMF)
#define DBG_REGS		(debug & DEBUG_REGS)
#define DBG_FOPS		(debug & DEBUG_FOPS)
#define DBG_EC			(debug & DEBUG_ECHOCAN)
#define DBG_ST			(debug & DEBUG_ST_STATE)
#define DBG_HDLC		(debug & DEBUG_HDLC)
#define DBG_ALARM		(debug & DEBUG_ALARM)

#define DBG_SPANFILTER		(BIT(bspan->port) & spanfilter)

static int debug = 0;
static int spanfilter = 0xFF; /* Bitmap for ports 1-8 */
#ifdef LOOPBACK_SUPPORTED
static int loopback = 0;
#endif
static int milliwatt = 0;
static int pedanticpci = 0;
static int teignorered = 0;
static int alarmdebounce = 500;
static int vpmsupport = 1;
static int timer_1_ms = 2000;
static int timer_3_ms = 30000;
static int alawoverride = 1;

#if !defined(mmiowb)
#define mmiowb() barrier()
#endif

#define MAX_B4_CARDS 64
static struct b4xxp *cards[MAX_B4_CARDS];

static int led_fader_table[] = {
	 0,  0,  0,  1,  2,  3,  4,  6,  8,  9, 11, 13, 16, 18, 20, 22, 24,
	25, 27, 28, 29, 30, 31, 31, 32, 31, 31, 30, 29, 28, 27, 25, 23, 22,
	20, 18, 16, 13, 11,  9,  8,  6,  4,  3,  2,  1,  0,  0,
};

// #define CREATE_WCB4XXP_PROCFS_ENTRY 
#ifdef CREATE_WCB4XXP_PROCFS_ENTRY
#define PROCFS_NAME 		"wcb4xxp"
static struct proc_dir_entry *myproc;
#endif

/* Expansion; right now there's just one card and all of its idiosyncrasies. */

#define FLAG_yyy	(1 << 0)
#define FLAG_zzz	(1 << 1)

struct devtype {
	char *desc;
	unsigned int flags;
	int ports;   	/* Number of ports the card has */
	enum cards_ids card_type;	/* Card type - Digium B410P, ... */
};

static struct devtype wcb4xxp =  {"Wildcard B410P", .ports = 4, .card_type = B410P  };
static struct devtype hfc2s =	 {"HFC-2S Junghanns.NET duoBRI PCI", .ports = 2, .card_type = DUOBRI };
static struct devtype hfc4s =	 {"HFC-4S Junghanns.NET quadBRI PCI", .ports = 4, .card_type = QUADBRI };
static struct devtype hfc8s =	 {"HFC-8S Junghanns.NET octoBRI PCI", .ports = 8, .card_type = OCTOBRI };
static struct devtype hfc2s_OV = {"OpenVox B200P", .ports = 2, .card_type = B200P_OV };
static struct devtype hfc4s_OV = {"OpenVox B400P", .ports = 4, .card_type = B400P_OV };
static struct devtype hfc8s_OV = {"OpenVox B800P", .ports = 8, .card_type = B800P_OV };
static struct devtype hfc2s_BN = {"BeroNet BN2S0", .ports = 2, .card_type = BN2S0 };
static struct devtype hfc4s_BN = {"BeroNet BN4S0", .ports = 4, .card_type = BN4S0 };
static struct devtype hfc8s_BN = {"BeroNet BN8S0", .ports = 8, .card_type = BN8S0 };
static struct devtype hfc4s_SW = {"Swyx 4xS0 SX2 QuadBri", .ports = 4, .card_type = BSWYX_SX2 };
static struct devtype hfc4s_EV = {"CCD HFC-4S Eval. Board", .ports = 4,
					.card_type = QUADBRI_EVAL };
 
#define CARD_HAS_EC(card) ((card)->card_type == B410P)

static int echocan_create(struct dahdi_chan *chan, struct dahdi_echocanparams *ecp,
			   struct dahdi_echocanparam *p, struct dahdi_echocan_state **ec);
static void echocan_free(struct dahdi_chan *chan, struct dahdi_echocan_state *ec);

static const struct dahdi_echocan_features my_ec_features = {
	.NLP_automatic = 1,
	.CED_tx_detect = 1,
	.CED_rx_detect = 1,
};

static const struct dahdi_echocan_ops my_ec_ops = {
	.name = "HWEC",
	.echocan_free = echocan_free,
};

#if 0
static const char *wcb4xxp_rcsdata = "$RCSfile: base.c,v $ $Revision$";
static const char *build_stamp = "" __DATE__ " " __TIME__ "";
#endif

/*
 * lowlevel PCI access functions
 * These are simply wrappers for the normal PCI access functions that the kernel provides,
 * except that they allow us to work around specific PCI quirks with module options.
 * Currently the only option supported is pedanticpci, which injects a (min.) 3us delay
 * after any PCI access to forcibly disable fast back-to-back transactions.
 * In the case of a PCI write, pedanticpci will also read from the status register, which
 * has the effect of flushing any pending PCI writes.
 */

static inline unsigned char __pci_in8(struct b4xxp *b4, const unsigned int reg)
{
	unsigned char ret = ioread8(b4->addr + reg);

#ifdef DEBUG_LOWLEVEL_REGS
	if (unlikely(DBG_REGS))
		drv_dbg(b4->dev, "read 0x%02x from 0x%p\n", ret, b4->addr + reg);
#endif
	if (unlikely(pedanticpci)) {
		udelay(3);
	}

	return ret;
}

static inline unsigned short __pci_in16(struct b4xxp *b4, const unsigned int reg)
{
	unsigned short ret = ioread16(b4->addr + reg);

#ifdef DEBUG_LOWLEVEL_REGS
	if (unlikely(DBG_REGS))
		drv_dbg(b4->dev, "read 0x%04x from 0x%p\n", ret, b4->addr + reg);
#endif
	if (unlikely(pedanticpci)) {
		udelay(3);
	}

	return ret;
}

static inline unsigned int __pci_in32(struct b4xxp *b4, const unsigned int reg)
{
	unsigned int ret = ioread32(b4->addr + reg);

#ifdef DEBUG_LOWLEVEL_REGS
	if (unlikely(DBG_REGS))
		drv_dbg(b4->dev, "read 0x%04x from 0x%p\n", ret, b4->addr + reg);
#endif
	if (unlikely(pedanticpci)) {
		udelay(3);
	}

	return ret;
}

static inline void __pci_out32(struct b4xxp *b4, const unsigned int reg, const unsigned int val)
{
#ifdef DEBUG_LOWLEVEL_REGS
	if (unlikely(DBG_REGS))
		drv_dbg(b4->dev, "writing 0x%02x to 0x%p\n", val, b4->addr + reg);
#endif
	iowrite32(val, b4->addr + reg);

	if (unlikely(pedanticpci)) {
		udelay(3);
		(void)ioread8(b4->addr + R_STATUS);
	}
}

static inline void __pci_out8(struct b4xxp *b4, const unsigned int reg, const unsigned char val)
{
#ifdef DEBUG_LOWLEVEL_REGS
	if (unlikely(DBG_REGS)) 
		drv_dbg(b4->dev, "writing 0x%02x to 0x%p\n", val, b4->addr + reg);
#endif
	iowrite8(val, b4->addr + reg);

	if (unlikely(pedanticpci)) {
		udelay(3);
		(void)ioread8(b4->addr + R_STATUS);
	}
}


/*
 * Standard I/O access functions
 * uses spinlocks to protect against multiple I/O accesses
 * DOES NOT automatically memory barrier
 */
static inline unsigned char b4xxp_getreg8(struct b4xxp *b4, const unsigned int reg)
{
	unsigned int ret;
	unsigned long irq_flags;

	spin_lock_irqsave(&b4->reglock, irq_flags);
#undef RETRY_REGISTER_READS
#ifdef RETRY_REGISTER_READS
	switch (reg) {
	case A_Z1:
	case A_Z1H:
	case A_F2:
	case R_IRQ_OVIEW:
	case R_BERT_STA:
	case A_ST_RD_STA:
	case R_IRQ_FIFO_BL0:
	case A_Z2:
	case A_Z2H:
	case A_F1:
	case R_RAM_USE:
	case R_F0_CNTL:
	case A_ST_SQ_RD:
	case R_IRQ_FIFO_BL7:
		/* On pg 53 of the data sheet for the hfc, it states that we must
		 * retry certain registers until we get two consecutive reads that are
		 * the same. */
retry:
		ret = __pci_in8(b4, reg);
		if (ret != __pci_in8(b4, reg)) 
			goto retry;
		break;
	default:
#endif
		ret = __pci_in8(b4, reg);
#ifdef RETRY_REGISTER_READS
		break;
	}
#endif
	spin_unlock_irqrestore(&b4->reglock, irq_flags);

#ifndef DEBUG_LOWLEVEL_REGS
	if (unlikely(DBG_REGS)) {
		dev_dbg(b4->dev, "read 0x%02x from 0x%p\n", ret, b4->addr + reg);
	}
#endif
	return ret;
}

static inline unsigned int b4xxp_getreg32(struct b4xxp *b4, const unsigned int reg)
{
	unsigned int ret;
	unsigned long irq_flags;

	spin_lock_irqsave(&b4->reglock, irq_flags);
	ret = __pci_in32(b4, reg);
	spin_unlock_irqrestore(&b4->reglock, irq_flags);

#ifndef DEBUG_LOWLEVEL_REGS
	if (unlikely(DBG_REGS)) {
		dev_dbg(b4->dev, "read 0x%04x from 0x%p\n", ret, b4->addr + reg);
	}
#endif
	return ret;
}

static inline unsigned short b4xxp_getreg16(struct b4xxp *b4, const unsigned int reg)
{
	unsigned int ret;
	unsigned long irq_flags;

	spin_lock_irqsave(&b4->reglock, irq_flags);
	ret = __pci_in16(b4, reg);
	spin_unlock_irqrestore(&b4->reglock, irq_flags);

#ifndef DEBUG_LOWLEVEL_REGS
	if (unlikely(DBG_REGS)) {
		dev_dbg(b4->dev, "read 0x%04x from 0x%p\n", ret, b4->addr + reg);
	}
#endif
	return ret;
}

static inline void b4xxp_setreg32(struct b4xxp *b4, const unsigned int reg, const unsigned int val)
{
	unsigned long irq_flags;

#ifndef DEBUG_LOWLEVEL_REGS
	if (unlikely(DBG_REGS)) {
		dev_dbg(b4->dev, "writing 0x%02x to 0x%p\n", val, b4->addr + reg);
	}
#endif
	spin_lock_irqsave(&b4->reglock, irq_flags);
	__pci_out32(b4, reg, val);
	spin_unlock_irqrestore(&b4->reglock, irq_flags);
}

static inline void b4xxp_setreg8(struct b4xxp *b4, const unsigned int reg, const unsigned char val)
{
	unsigned long irq_flags;

#ifndef DEBUG_LOWLEVEL_REGS
	if (unlikely(DBG_REGS)) {
		dev_dbg(b4->dev, "writing 0x%02x to 0x%p\n", val, b4->addr + reg);
	}
#endif
	spin_lock_irqsave(&b4->reglock, irq_flags);
	__pci_out8(b4, reg, val);
	spin_unlock_irqrestore(&b4->reglock, irq_flags);
}

/*
 * A lot of the registers in the HFC are indexed.
 * this function sets the index, and then writes to the indexed register in an ordered fashion.
 * memory barriers are useless unless spinlocked, so that's what these wrapper functions do.
 */
static void b4xxp_setreg_ra(struct b4xxp *b4, unsigned char r, unsigned char rd, unsigned char a, unsigned char ad)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&b4->seqlock, irq_flags);

	b4xxp_setreg8(b4, r, rd);
	wmb();
	b4xxp_setreg8(b4, a, ad);

	mmiowb();
	spin_unlock_irqrestore(&b4->seqlock, irq_flags);
}

static unsigned char b4xxp_getreg_ra(struct b4xxp *b4, unsigned char r, unsigned char rd, unsigned char a)
{
	unsigned long irq_flags;
	unsigned char val;

	spin_lock_irqsave(&b4->seqlock, irq_flags);

	b4xxp_setreg8(b4, r, rd);
	wmb();
	val = b4xxp_getreg8(b4, a);
	
	mmiowb();
	spin_unlock_irqrestore(&b4->seqlock, irq_flags);
	return val;
}


/*
 * HFC-4S GPIO routines
 *
 * the B410P uses the HFC-4S GPIO as follows:
 * GPIO 8..10: output, CPLD register select
 * GPIO12..15: output, 1 = enable power for port 1-4
 * GPI16: input, 0 = echo can #1 interrupt
 * GPI17: input, 0 = echo can #2 interrupt
 * GPI23: input, 1 = NT power module installed
 * GPI24..27: input, NT power module problem on port 1-4
 * GPI28..31: input, 1 = port 1-4 in NT mode
 */

/* initialize HFC-4S GPIO. Set up pin drivers before setting GPIO mode */
static void hfc_gpio_init(struct b4xxp *b4)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&b4->seqlock, irq_flags);

	flush_pci();				/* flush any pending PCI writes */

	mb();

	b4xxp_setreg8(b4, R_GPIO_EN0, 0x00);	/* GPIO0..7 input */
	b4xxp_setreg8(b4, R_GPIO_EN1, 0xf7);	/* GPIO8..10,12..15 outputs, GPIO11 input */
	b4xxp_setreg8(b4, R_GPIO_OUT1, 0x00);	/* disable power, CPLD reg 0 */

	mb();

	switch (b4->card_type) {
	case OCTOBRI:  /* fall through */
	case B800P_OV: /* fall through */
	case BN8S0:
		/* GPIO0..15 S/T - HFC-8S uses GPIO8-15 for S/T ports 5-8 */
		b4xxp_setreg8(b4, R_GPIO_SEL, 0x00);
		break;
	default:
		/* GPIO0..7 S/T, 8..15 GPIO */
		b4xxp_setreg8(b4, R_GPIO_SEL, 0xf0);
		break;
	}

	mb();

	spin_unlock_irqrestore(&b4->seqlock, irq_flags);
}

/*
 * HFC SRAM interface code.
 * This came from mattf, I don't even pretend to understand it,
 * It seems to be using undocumented features in the HFC.
 * I just added the __pci_in8() to ensure the PCI writes made it
 * to hardware by the time these functions return. 
 */
static inline void enablepcibridge(struct b4xxp *b4)
{
	b4xxp_setreg8(b4, R_BRG_PCM_CFG, 0x03);
	flush_pci();
}

static inline void disablepcibridge(struct b4xxp *b4)
{
	b4xxp_setreg8(b4, R_BRG_PCM_CFG, 0x02);
	flush_pci();
}

/* NOTE: read/writepcibridge do not use __pci_in/out because they are using b4->ioaddr not b4->addr */
static inline unsigned char readpcibridge(struct b4xxp *b4, unsigned char address)
{
	unsigned short cipv;
	unsigned char data;


/* slow down a PCI read access by 1 PCI clock cycle */
	b4xxp_setreg8(b4, R_CTRL, 0x4);
	wmb();

	if (address == 0)
		cipv=0x4000;
	else
		cipv=0x5800;

/* select local bridge port address by writing to CIP port */
	iowrite16(cipv, b4->ioaddr + 4);
	wmb();
	data = ioread8(b4->ioaddr);

/* restore R_CTRL for normal PCI read cycle speed */
	b4xxp_setreg8(b4, R_CTRL, 0x0);
	wmb();
	flush_pci();

	return data;
}

static inline void writepcibridge(struct b4xxp *b4, unsigned char address, unsigned char data)
{
	unsigned short cipv;
	unsigned int datav;


	if (address == 0)
		cipv=0x4000;
	else
		cipv=0x5800;

/* select local bridge port address by writing to CIP port */
	iowrite16(cipv, b4->ioaddr + 4);
	wmb();

/* define a 32 bit dword with 4 identical bytes for write sequence */
	datav = data | ( (__u32) data <<8) | ( (__u32) data <<16) | ( (__u32) data <<24);

/*
 * write this 32 bit dword to the bridge data port
 * this will initiate a write sequence of up to 4 writes to the same address on the local bus
 * interface
 * the number of write accesses is undefined but >=1 and depends on the next PCI transaction
 * during write sequence on the local bus
 */
	iowrite32(datav, b4->ioaddr);
	wmb();
	flush_pci();
}

/* CPLD access code, more or less copied verbatim from code provided by mattf. */
static inline void cpld_select_reg(struct b4xxp *b4, unsigned char reg)
{
	b4xxp_setreg8(b4, R_GPIO_OUT1, reg);
	flush_pci();
}

static inline void cpld_setreg(struct b4xxp *b4, unsigned char reg, unsigned char val)
{
	cpld_select_reg(b4, reg);

	enablepcibridge(b4);
	writepcibridge(b4, 1, val);
	disablepcibridge(b4);
}

static inline unsigned char cpld_getreg(struct b4xxp *b4, unsigned char reg)
{
	unsigned char data;

	cpld_select_reg(b4, reg);

	enablepcibridge(b4);
	data = readpcibridge(b4, 1);
	disablepcibridge(b4);

	return data;
}


/*
 * echo canceller code, verbatim from mattf.
 * I don't pretend to understand it.
 */
static inline void ec_select_addr(struct b4xxp *b4, unsigned short addr)
{
	cpld_setreg(b4, 0, 0xff & addr);
	cpld_setreg(b4, 1, 0x01 & (addr >> 8));
}

static inline unsigned short ec_read_data(struct b4xxp *b4)
{
	unsigned short addr;
	unsigned short highbit;
	
	addr = cpld_getreg(b4, 0);
	highbit = cpld_getreg(b4, 1);

	addr = addr | (highbit << 8);

	return addr & 0x1ff;
}

static inline unsigned char ec_read(struct b4xxp *b4, int which, unsigned short addr)
{
	unsigned char data;
	unsigned long flags;

	spin_lock_irqsave(&b4->seqlock, flags);
	ec_select_addr(b4, addr);

	if (!which)
		cpld_select_reg(b4, 2);
	else
		cpld_select_reg(b4, 3);

	enablepcibridge(b4);
	data = readpcibridge(b4, 1);
	disablepcibridge(b4);

	cpld_select_reg(b4, 0);
	spin_unlock_irqrestore(&b4->seqlock, flags);

	return data;
}

static inline void ec_write(struct b4xxp *b4, int which, unsigned short addr, unsigned char data)
{
	unsigned char in;
	unsigned long flags;

	spin_lock_irqsave(&b4->seqlock, flags);

	ec_select_addr(b4, addr);

	enablepcibridge(b4);

	if (!which)
		cpld_select_reg(b4, 2);
	else
		cpld_select_reg(b4, 3);

	writepcibridge(b4, 1, data);
	cpld_select_reg(b4, 0);
	disablepcibridge(b4);

	spin_unlock_irqrestore(&b4->seqlock, flags);

	in = ec_read(b4, which, addr);

	if (in != data) {
		if (printk_ratelimit()) {
			dev_warn(b4->dev, "ec_write: Wrote 0x%02x to register 0x%02x "
			         "of VPM %d but got back 0x%02x\n", data, addr, which, in);
		}
	}
}

#define NUM_EC 2
#define MAX_TDM_CHAN 32

#if 0
void ec_set_dtmf_threshold(struct b4xxp *b4, int threshold)
{
        unsigned int x;

        for (x = 0; x < NUM_EC; x++) {
                ec_write(b4, x, 0xC4, (threshold >> 8) & 0xFF);
                ec_write(b4, x, 0xC5, (threshold & 0xFF));
        }
        printk("VPM: DTMF threshold set to %d\n", threshold);
}
#endif


static void ec_init(struct b4xxp *b4)
{
	unsigned char b;
	unsigned int i, j, mask;

	if (!CARD_HAS_EC(b4))
		return;

/* Setup GPIO */
	for (i=0; i < NUM_EC; i++) {
		b = ec_read(b4, i, 0x1a0);

		dev_info(b4->dev, "VPM %d/%d init: chip ver %02x\n", i, NUM_EC - 1, b);

		for (j=0; j < b4->numspans; j++) {
			ec_write(b4, i, 0x1a8 + j, 0x00);	/* GPIO out */
			ec_write(b4, i, 0x1ac + j, 0x00);	/* GPIO dir */
			ec_write(b4, i, 0x1b0 + j, 0x00);	/* GPIO sel */
		}

/* Setup TDM path - sets fsync and tdm_clk as inputs */
		b = ec_read(b4, i, 0x1a3);			/* misc_con */
		ec_write(b4, i, 0x1a3, b & ~0x02);

/* Setup Echo length (512 taps) */
		ec_write(b4, i, 0x022, 1);
		ec_write(b4, i, 0x023, 0xff);

/* Setup timeslots */
		ec_write(b4, i, 0x02f, 0x00);
		mask = 0x02020202 << (i * 4);

/* Setup the tdm channel masks for all chips*/
		for (j=0; j < 4; j++)
			ec_write(b4, i, 0x33 - j, (mask >> (j << 3)) & 0xff);

/* Setup convergence rate */
		if (DBG)
			dev_info(b4->dev, "setting A-law mode\n");

		b = ec_read(b4, i, 0x20);
		b &= 0xe0;
		b |= 0x12;
		if (alawoverride) {
			b |= 0x01;
		}
		ec_write(b4, i, 0x20, b);
		if (DBG)
			dev_info(b4->dev, "reg 0x20 is 0x%02x\n", b);

//		ec_write(b4, i, 0x20, 0x38);

#if 0
		ec_write(b4, i, 0x24, 0x02);
		b = ec_read(b4, i, 0x24);
#endif
		if (DBG)
			dev_info(b4->dev, "NLP threshold is set to %d (0x%02x)\n", b, b);

/* Initialize echo cans */
		for (j=0; j < MAX_TDM_CHAN; j++) {
			if (mask & (0x00000001 << j))
				ec_write(b4, i, j, 0x00);
		}

		mdelay(10);

/* Put in bypass mode */
		for (j=0; j < MAX_TDM_CHAN; j++) {
			if (mask & (0x00000001 << j)) {
				ec_write(b4, i, j, 0x01);
			}
		}

/* Enable bypass */
		for (j=0; j < MAX_TDM_CHAN; j++) {
			if (mask & (0x00000001 << j))
				ec_write(b4, i, 0x78 + j, 0x01);
		}
	}

#if 0
	ec_set_dtmf_threshold(b4, 1250);
#endif
}

/* performs a register write and then waits for the HFC "busy" bit to clear */
static void hfc_setreg_waitbusy(struct b4xxp *b4, const unsigned int reg, const unsigned int val)
{
	int timeout = 0;
	unsigned long start;
	const int TIMEOUT = HZ/4; /* 250ms */

	start = jiffies;
	while (unlikely((b4xxp_getreg8(b4, R_STATUS) & V_BUSY))) {
		if (time_after(jiffies, start + TIMEOUT)) {
			timeout = 1;
			break;
		}
	};

	mb();
	b4xxp_setreg8(b4, reg, val);
	mb();

	start = jiffies;
	while (likely((b4xxp_getreg8(b4, R_STATUS) & V_BUSY))) {
		if (time_after(jiffies, start + TIMEOUT)) {
			timeout = 1;
			break;
		}
	};

	if (timeout) {
		if (printk_ratelimit())
			dev_warn(b4->dev, "hfc_setreg_waitbusy(write 0x%02x to 0x%02x) timed out waiting for busy flag to clear!\n", val, reg);
	}
}

/*
 * reads an 8-bit register over over and over until the same value is read twice, then returns that value.
 */
static inline unsigned char hfc_readcounter8(struct b4xxp *b4, const unsigned int reg)
{
	unsigned char r1, r2;
	unsigned long maxwait = 1048576;

	do {
		r1 = b4xxp_getreg8(b4, reg);
		r2 = b4xxp_getreg8(b4, reg);
	} while ((r1 != r2) && maxwait--);

	if (!maxwait) {
		if (printk_ratelimit())
			dev_warn(b4->dev, "hfc_readcounter8(reg 0x%02x) timed out waiting for data to settle!\n", reg);
	}

	return r1;
}

/*
 * reads a 16-bit register over over and over until the same value is read twice, then returns that value.
 */
static inline unsigned short hfc_readcounter16(struct b4xxp *b4, const unsigned int reg)
{
	unsigned short r1, r2;
	unsigned long maxwait = 1048576;

	do {
		r1 = b4xxp_getreg16(b4, reg);
		r2 = b4xxp_getreg16(b4, reg);
	} while ((r1 != r2) && maxwait--);

	if (!maxwait) {
		if (printk_ratelimit())
			dev_warn(b4->dev, "hfc_readcounter16(reg 0x%02x) timed out waiting for data to settle!\n", reg);
	}

	return r1;
}

static inline unsigned int hfc_readcounter32(struct b4xxp *b4, const unsigned int reg)
{
	unsigned int r1, r2;
	unsigned long maxwait = 1048576;

	do {
		r1 = b4xxp_getreg32(b4, reg);
		r2 = b4xxp_getreg32(b4, reg);
	} while ((r1 != r2) && maxwait--);

	if (!maxwait) {
		if (printk_ratelimit())
			dev_warn(b4->dev, "hfc_readcounter32(reg 0x%02x) timed out waiting for data to settle!\n", reg);
	}

	return r1;
}

/* performs a soft-reset of the HFC-4S.  This is as clean-slate as you can get to a hardware reset. */
static void hfc_reset(struct b4xxp *b4)
{
	int b, c;

/* all 32 FIFOs the same size (384 bytes), channel select data flow mode, sized for internal RAM */
	b4xxp_setreg8(b4, R_FIFO_MD, V_FIFO_MD_00 | V_DF_MD_CSM | V_FIFO_SZ_00);
	flush_pci();

/* reset everything, wait 500us, then bring everything BUT the PCM system out of reset */
	b4xxp_setreg8(b4, R_CIRM, HFC_FULL_RESET);
	flush_pci();

	udelay(500);

	b4xxp_setreg8(b4, R_CIRM, V_PCM_RES);
	flush_pci();

	udelay(500);

/*
 * Now bring PCM out of reset and do a very basic setup of the PCM system to allow it to finish resetting correctly.
 * set F0IO as an output, and set up a 32-timeslot PCM bus
 * See Section 8.3 in the HFC-4S datasheet for more details.
 */
	b4xxp_setreg8(b4, R_CIRM, 0x00);
	b4xxp_setreg8(b4, R_PCM_MD0, V_PCM_MD | V_PCM_IDX_MD1);
	flush_pci();

	b4xxp_setreg8(b4, R_PCM_MD1, V_PLL_ADJ_00 | V_PCM_DR_2048);
	flush_pci();

/* now wait for R_F0_CNTL to reach at least 2 before continuing */
	c=10;
	while ((b = b4xxp_getreg8(b4, R_F0_CNTL)) < 2 && c) { udelay(100); c--; }

	if (!c && b < 2) {
		dev_warn(b4->dev, "hfc_reset() did not get the green light from the PCM system!\n");
	}
}

static inline void hfc_enable_fifo_irqs(struct b4xxp *b4)
{
	b4xxp_setreg8(b4, R_IRQ_CTRL, V_FIFO_IRQ | V_GLOB_IRQ_EN);
	flush_pci();
}

static inline void hfc_disable_fifo_irqs(struct b4xxp *b4)
{
	b4xxp_setreg8(b4, R_IRQ_CTRL, V_GLOB_IRQ_EN);
	flush_pci();
}

static void hfc_enable_interrupts(struct b4xxp *b4)
{
	b4->running = 1;

/* clear any pending interrupts */
	b4xxp_getreg8(b4, R_STATUS);
	b4xxp_getreg8(b4, R_IRQ_MISC);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL0);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL1);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL2);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL3);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL4);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL5);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL6);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL7);

	b4xxp_setreg8(b4, R_IRQMSK_MISC, V_TI_IRQ);
	hfc_enable_fifo_irqs(b4);
}

static void hfc_disable_interrupts(struct b4xxp *b4)
{
	b4xxp_setreg8(b4, R_IRQMSK_MISC, 0);
	b4xxp_setreg8(b4, R_IRQ_CTRL, 0);
	flush_pci();
	b4->running = 0;
}

/*
 * Connects an S/T port's B channel to a host-facing FIFO through the PCM busses.
 * This bchan flow plan should match up with the EC requirements.
 * TODO: Interrupts are only enabled on the host FIFO RX side, since everything is (should be) synchronous.
 * *** performs no error checking of parameters ***
 */
static void hfc_assign_bchan_fifo_ec(struct b4xxp *b4, int port, int bchan)
{
	int fifo, hfc_chan, ts;
	unsigned long irq_flags;
	static int first=1;

	if (first) {
		first = 0;
		dev_info(b4->dev, "Hardware echo cancellation enabled.\n");
	}

	fifo = port * 2;
	hfc_chan = port * 4;
	ts = port * 8;

	if (bchan) {
		fifo += 1;
		hfc_chan += 1;
		ts += 4;
	}

/* record the host's FIFO # in the span fifo array */
	b4->spans[port].fifos[bchan] = fifo;
	spin_lock_irqsave(&b4->fifolock, irq_flags);

	if (DBG)
		dev_info(b4->dev, "port %d, B channel %d\n\tS/T -> PCM ts %d uses HFC chan %d via FIFO %d\n", port, bchan, ts + 1, hfc_chan, 16 + fifo);

/* S/T RX -> PCM TX FIFO, transparent mode, no IRQ. */
	hfc_setreg_waitbusy(b4, R_FIFO, ((16 + fifo) << V_FIFO_NUM_SHIFT));
	b4xxp_setreg8(b4, A_CON_HDLC, V_IFF | V_HDLC_TRP | V_DATA_FLOW_110);
	b4xxp_setreg8(b4, A_CHANNEL, (hfc_chan << V_CH_FNUM_SHIFT));
	b4xxp_setreg8(b4, R_SLOT, ((ts + 1) << V_SL_NUM_SHIFT));
	b4xxp_setreg8(b4, A_SL_CFG, V_ROUT_TX_STIO1 | (hfc_chan << V_CH_SNUM_SHIFT));
	hfc_setreg_waitbusy(b4, A_INC_RES_FIFO, V_RES_FIFO);

	if (DBG)
		pr_info("\tPCM ts %d -> host uses HFC chan %d via FIFO %d\n", ts + 1, 16 + hfc_chan, fifo);

/* PCM RX -> Host TX FIFO, transparent mode, enable IRQ. */
	hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT) | V_FIFO_DIR);
	b4xxp_setreg8(b4, A_CON_HDLC, V_IFF | V_HDLC_TRP | V_DATA_FLOW_001); 
	b4xxp_setreg8(b4, A_CHANNEL, ((16 + hfc_chan) << V_CH_FNUM_SHIFT) | V_CH_FDIR);
	b4xxp_setreg8(b4, R_SLOT, ((ts + 1) << V_SL_NUM_SHIFT) | 1);
	b4xxp_setreg8(b4, A_SL_CFG, V_ROUT_RX_STIO2 | ((16 + hfc_chan) << V_CH_SNUM_SHIFT) | 1);
	hfc_setreg_waitbusy(b4, A_INC_RES_FIFO, V_RES_FIFO);
//	b4xxp_setreg8(b4, A_IRQ_MSK, V_IRQ);

	if (DBG)
		pr_info("\thost -> PCM ts %d uses HFC chan %d via FIFO %d\n", ts, 16 + hfc_chan, fifo);

/* Host FIFO -> PCM TX */
	hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT));
	b4xxp_setreg8(b4, A_CON_HDLC, V_IFF | V_HDLC_TRP | V_DATA_FLOW_001);
	b4xxp_setreg8(b4, A_CHANNEL, ((16 + hfc_chan) << V_CH_FNUM_SHIFT));
	b4xxp_setreg8(b4, R_SLOT, (ts << V_SL_NUM_SHIFT));
	b4xxp_setreg8(b4, A_SL_CFG, V_ROUT_RX_STIO2 | ((16 + hfc_chan) << V_CH_SNUM_SHIFT));
	hfc_setreg_waitbusy(b4, A_INC_RES_FIFO, V_RES_FIFO);

	if (DBG)
		pr_info("\tPCM ts %d -> S/T uses HFC chan %d via FIFO %d\n", ts, hfc_chan, 16 + fifo);

/* PCM -> S/T */
	hfc_setreg_waitbusy(b4, R_FIFO, ((16 + fifo) << V_FIFO_NUM_SHIFT) | V_FIFO_DIR);
	b4xxp_setreg8(b4, A_CON_HDLC, V_IFF | V_HDLC_TRP | V_DATA_FLOW_110);
	b4xxp_setreg8(b4, A_CHANNEL, (hfc_chan << V_CH_FNUM_SHIFT) | V_CH_FDIR);
	b4xxp_setreg8(b4, R_SLOT, (ts  << V_SL_NUM_SHIFT) | V_SL_DIR);
	b4xxp_setreg8(b4, A_SL_CFG, V_ROUT_TX_STIO2 | (hfc_chan << V_CH_SNUM_SHIFT) | V_CH_SDIR);
	hfc_setreg_waitbusy(b4, A_INC_RES_FIFO, V_RES_FIFO);

	if (DBG)
		pr_info("\tPCM ts %d -> S/T uses HFC chan %d via FIFO %d\n", ts, hfc_chan, 16 + fifo);

	flush_pci();			/* ensure all those writes actually hit hardware */
	spin_unlock_irqrestore(&b4->fifolock, irq_flags);
}

static void hfc_assign_bchan_fifo_noec(struct b4xxp *b4, int port, int bchan)
{
	int fifo, hfc_chan, ts;
	unsigned long irq_flags;
	static int first=1;

	if (first) {
		first = 0;
		dev_info(b4->dev, "NOTE: hardware echo cancellation has been disabled\n");
	}

	fifo = port * 2;
	hfc_chan = port * 4;
	ts = port * 8;

	if (bchan) {
		fifo += 1;
		hfc_chan += 1;
		ts += 4;
	}

/* record the host's FIFO # in the span fifo array */
	b4->spans[port].fifos[bchan] = fifo;
	spin_lock_irqsave(&b4->fifolock, irq_flags);

	if (DBG)
		dev_info(b4->dev, "port %d, B channel %d\n\thost -> S/T uses HFC chan %d via FIFO %d\n", port, bchan, hfc_chan, fifo);

	hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT));
	b4xxp_setreg8(b4, A_CON_HDLC, V_IFF | V_HDLC_TRP | V_DATA_FLOW_000);
	b4xxp_setreg8(b4, A_CHANNEL, (hfc_chan << V_CH_FNUM_SHIFT));
	hfc_setreg_waitbusy(b4, A_INC_RES_FIFO, V_RES_FIFO);

	if (DBG)
		pr_info("\tS/T -> host uses HFC chan %d via FIFO %d\n", hfc_chan, fifo);

	hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT) | V_FIFO_DIR);
	b4xxp_setreg8(b4, A_CON_HDLC, V_IFF | V_HDLC_TRP | V_DATA_FLOW_000);
	b4xxp_setreg8(b4, A_CHANNEL, (hfc_chan << V_CH_FNUM_SHIFT) | V_CH_FDIR);
	hfc_setreg_waitbusy(b4, A_INC_RES_FIFO, V_RES_FIFO);

	if (DBG)
		pr_info("\tPCM ts %d -> S/T uses HFC chan %d via FIFO %d\n", ts, hfc_chan, 16 + fifo);

	flush_pci();			/* ensure all those writes actually hit hardware */
	spin_unlock_irqrestore(&b4->fifolock, irq_flags);
}

/*
 * Connects an S/T port's D channel to a host-facing FIFO.
 * Both TX and RX interrupts are enabled!
 * *** performs no error checking of parameters ***
 */
static void hfc_assign_dchan_fifo(struct b4xxp *b4, int port)
{
	int fifo, hfc_chan;
	unsigned long irq_flags;

	switch (b4->card_type) {
	case B800P_OV: /* fall through */
	case OCTOBRI: /* fall through */
	case BN8S0:
		/* In HFC-8S cards we can't use ports 8-11 for dchan FIFOs */
		fifo = port + 16;
		break;
	default:
		fifo = port + 8;
		break;
	}

	hfc_chan = (port * 4) + 2;

/* record the host's FIFO # in the span fifo array */
	b4->spans[port].fifos[2] = fifo;

	if (DBG)
		dev_info(b4->dev, "port %d, D channel\n\thost -> S/T uses HFC chan %d via FIFO %d\n", port, hfc_chan, fifo);

	spin_lock_irqsave(&b4->fifolock, irq_flags);

/* Host FIFO -> S/T TX, HDLC mode, no IRQ. */
	hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT));
	b4xxp_setreg8(b4, A_CON_HDLC, V_IFF | V_TRP_IRQ | V_DATA_FLOW_000);
	b4xxp_setreg8(b4, A_CHANNEL, (hfc_chan << V_CH_FNUM_SHIFT));
	b4xxp_setreg8(b4, A_SUBCH_CFG, 0x02);
	hfc_setreg_waitbusy(b4, A_INC_RES_FIFO, V_RES_FIFO);

	if (DBG)
		pr_info("\tS/T -> host uses HFC chan %d via FIFO %d\n", hfc_chan, fifo);

/* S/T RX -> Host FIFO, HDLC mode, IRQ will be enabled when port opened. */
	hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT) | V_FIFO_DIR);
	b4xxp_setreg8(b4, A_CON_HDLC, V_IFF | V_TRP_IRQ | V_DATA_FLOW_000);
	b4xxp_setreg8(b4, A_CHANNEL, (hfc_chan << V_CH_FNUM_SHIFT) | V_CH_FDIR);
	b4xxp_setreg8(b4, A_SUBCH_CFG, 0x02);
	hfc_setreg_waitbusy(b4, A_INC_RES_FIFO, V_RES_FIFO);

	if (DBG)
		pr_info("\n");

	flush_pci();			/* ensure all those writes actually hit hardware */
	spin_unlock_irqrestore(&b4->fifolock, irq_flags);
}

/* takes a read/write fifo pair and optionally resets it, optionally enabling the rx/tx interrupt */
static void hfc_reset_fifo_pair(struct b4xxp *b4, int fifo, int reset, int force_no_irq)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&b4->fifolock, irq_flags);

	hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT));
	b4xxp_setreg8(b4, A_IRQ_MSK, (!force_no_irq && b4->fifo_en_txint & (1 << fifo)) ? V_IRQ : 0);

	if (reset)
		hfc_setreg_waitbusy(b4, A_INC_RES_FIFO, V_RES_FIFO);

	hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT) | V_FIFO_DIR);
	b4xxp_setreg8(b4, A_IRQ_MSK, (!force_no_irq && b4->fifo_en_rxint & (1 << fifo)) ? V_IRQ : 0);

	if (reset)
		hfc_setreg_waitbusy(b4, A_INC_RES_FIFO, V_RES_FIFO);

	spin_unlock_irqrestore(&b4->fifolock, irq_flags);
}


static void b4xxp_set_sync_src(struct b4xxp *b4, int port)
{
	int b;

	if (port == -1) 		/* automatic */
		b = 0;
	else
		b = (port & V_SYNC_SEL_MASK) | V_MAN_SYNC;

	b4xxp_setreg8(b4, R_ST_SYNC, b);
}

/*
 * Finds the highest-priority sync span that is not in alarm and returns it.
 * Note: the span #s in b4->spans[].sync are 1-based, and this returns
 * a 0-based span, or -1 if no spans are found.
 */
static int b4xxp_find_sync(struct b4xxp *b4)
{
	int i, psrc, src;

	src = -1;		/* default to automatic */

	for (i=0; i < b4->numspans; i++) {
		psrc = b4->spans[i].sync;
		if (psrc > 0 && !b4->spans[psrc - 1].span.alarms) {
			src = psrc;
			break;
		}
	}

	return src - 1;
}

/*
 * allocates memory and pretty-prints a given S/T state engine state to it.
 * calling routine is responsible for freeing the pointer returned!
 * Performs no hardware access whatsoever, but does use GFP_KERNEL so do not call from IRQ context.
 * if full == 1, prints a "full" dump; otherwise just prints current state.
 */
static char *hfc_decode_st_state(struct b4xxp *b4, int port, unsigned char state, int full)
{
	int nt, sta;
	char s[128], *str;
	const char *ststr[2][16] = {	/* TE, NT */
		{ "RESET", "?", "SENSING", "DEACT.", "AWAIT.SIG", "IDENT.INPUT", "SYNCD", "ACTIVATED",
			"LOSTFRAMING", "?", "?", "?", "?", "?", "?", "?" },
		{ "RESET", "DEACT.", "PEND.ACT", "ACTIVE", "PEND.DEACT", "?", "?", "?",
			"?", "?", "?", "?", "?", "?", "?", "?" }
	};

	if (!(str = kmalloc(256, GFP_KERNEL))) {
		dev_warn(b4->dev, "could not allocate mem for ST state decode string!\n");
		return NULL;
	}

	nt = (b4->spans[port].te_mode == 0);
	sta = (state & V_ST_STA_MASK);

	sprintf(str, "P%d: %s state %c%d (%s)", port + 1, (nt ? "NT" : "TE"), (nt ? 'G' : 'F'), sta, ststr[nt][sta]);

	if (full) {
		sprintf(s, " SYNC: %s, RX INFO0: %s", ((state & V_FR_SYNC) ? "yes" : "no"), ((state & V_INFO0) ? "yes" : "no"));
		strcat(str, s);

		if (nt) {
			sprintf(s, ", T2 %s, auto G2->G3: %s", ((state & V_T2_EXP) ? "expired" : "OK"),
				((state & V_G2_G3) ? "yes" : "no"));
			strcat(str, s);
		}
	}

	return str;
}

/*
 * sets an S/T port state machine to a given state.
 * if 'auto' is nonzero, will put the state machine back in auto mode after setting the state.
 */
static void hfc_handle_state(struct b4xxp_span *s);
static void hfc_force_st_state(struct b4xxp *b4, int port, int state, int resume_auto)
{
	b4xxp_setreg_ra(b4, R_ST_SEL, port, A_ST_RD_STA, state | V_ST_LD_STA);

	udelay(6);

	if (resume_auto) {
		b4xxp_setreg_ra(b4, R_ST_SEL, port, A_ST_RD_STA, state);
	}

	if (DBG_ST) {
		char *x;

		x = hfc_decode_st_state(b4, port, state, 1);
		dev_info(b4->dev, "forced port %d to state %d (auto: %d), new decode: %s\n", port + 1, state, resume_auto, x);
		kfree(x);
	}

/* make sure that we activate any timers/etc needed by this state change */
	hfc_handle_state(&b4->spans[port]);
}

/* figures out what to do when an S/T port's timer expires. */
static void hfc_timer_expire(struct b4xxp_span *s, int t_no)
{
	struct b4xxp *b4 = s->parent;

	if (DBG_ST)
		dev_info(b4->dev, "%lu: hfc_timer_expire, Port %d T%d expired (value=%lu ena=%d)\n", b4->ticks, s->port + 1, t_no + 1, s->hfc_timers[t_no], s->hfc_timer_on[t_no]);
/*
 * there are three timers associated with every HFC S/T port.
 * T1 is used by the NT state machine, and is the maximum time the NT side should wait for G3 (active) state.
 * T2 is not actually used in the driver, it is handled by the HFC-4S internally.
 * T3 is used by the TE state machine; it is the maximum time the TE side should wait for the INFO4 (activated) signal.
 */

/* First, disable the expired timer; hfc_force_st_state() may activate it again. */
	s->hfc_timer_on[t_no] = 0;

	switch(t_no) {
	case HFC_T1:					/* switch to G4 (pending deact.), resume auto mode */
		hfc_force_st_state(b4, s->port, 4, 1);
		break;
	case HFC_T2:					/* switch to G1 (deactivated), resume auto mode */
		hfc_force_st_state(b4, s->port, 1, 1);
		break;
	case HFC_T3:					/* switch to F3 (deactivated), resume auto mode */
		hfc_force_st_state(b4, s->port, 3, 1);
		break;
	default:
		if (printk_ratelimit())
			dev_warn(b4->dev, "hfc_timer_expire found an unknown expired timer (%d)??\n", t_no);
	}
}

/*
 * Run through the active timers on a card and deal with any expiries.
 * Also see if the alarm debounce time has expired and if it has, tell DAHDI.
 */
static void hfc_update_st_timers(struct b4xxp *b4)
{
	int i, j;
	struct b4xxp_span *s;

	for (i=0; i < b4->numspans; i++) {
		s = &b4->spans[i];

		for (j=HFC_T1; j <= HFC_T3; j++) {

/* we don't really do timer2, it is expired by the state change handler */
			if (j == HFC_T2)
				continue;

			if (s->hfc_timer_on[j] && time_after_eq(b4->ticks, s->hfc_timers[j])) {
				hfc_timer_expire(s, j);
			}
		}

		if (s->newalarm != s->span.alarms && time_after_eq(b4->ticks, s->alarmtimer)) {
			if (!s->te_mode || !teignorered) {
				s->span.alarms = s->newalarm;
				dahdi_alarm_notify(&s->span);
				if (DBG_ALARM)
					dev_info(b4->dev, "span %d: alarm %d debounced\n", i + 1, s->newalarm);
				if (!s->te_mode)
					b4xxp_set_sync_src(b4, b4xxp_find_sync(b4));
			}
		}
	}
}

/* this is the driver-level state machine for an S/T port */
static void hfc_handle_state(struct b4xxp_span *s)
{
	struct b4xxp *b4;
	unsigned char state, sta;
	int nt, newsync, oldalarm;
	unsigned long oldtimer;

	b4 = s->parent;
	nt = !s->te_mode;

	state = b4xxp_getreg_ra(b4, R_ST_SEL, s->port, A_ST_RD_STA);
	sta = (state & V_ST_STA_MASK);

	if (DBG_ST) {
		char *x;

		x = hfc_decode_st_state(b4, s->port, state, 1);
		dev_info(b4->dev, "port %d A_ST_RD_STA old=0x%02x now=0x%02x, decoded: %s\n", s->port + 1, s->oldstate, state, x);
		kfree(x);
	}

	oldalarm = s->newalarm;
	oldtimer = s->alarmtimer;

	if (nt) {
		switch(sta) {
		default:			/* Invalid NT state */
		case 0x0:			/* NT state G0: Reset */
		case 0x1:			/* NT state G1: Deactivated */
		case 0x4:			/* NT state G4: Pending Deactivation */
			s->newalarm = DAHDI_ALARM_RED;
			break;
		case 0x2:			/* NT state G2: Pending Activation */
			s->newalarm = DAHDI_ALARM_YELLOW;
			break;
		case 0x3:			/* NT state G3: Active */
			s->hfc_timer_on[HFC_T1] = 0;
			s->newalarm = 0;
			break;
		}
	} else {
		switch(sta) {
		default:			/* Invalid TE state */
		case 0x0:			/* TE state F0: Reset */
		case 0x2:			/* TE state F2: Sensing */
		case 0x3:			/* TE state F3: Deactivated */
		case 0x4:			/* TE state F4: Awaiting Signal */
		case 0x8:			/* TE state F8: Lost Framing */
			s->newalarm = DAHDI_ALARM_RED;
			break;
		case 0x5:			/* TE state F5: Identifying Input */
		case 0x6:			/* TE state F6: Synchronized */
			s->newalarm = DAHDI_ALARM_YELLOW;
			break;
		case 0x7:			/* TE state F7: Activated */
			s->hfc_timer_on[HFC_T3] = 0;
			s->newalarm = 0;
			break;
		}
	}

	s->alarmtimer = b4->ticks + alarmdebounce;
	s->oldstate = state;

	if (DBG_ALARM) {
		dev_info(b4->dev, "span %d: old alarm %d expires %ld, new alarm %d expires %ld\n",
			s->port + 1, oldalarm, oldtimer, s->newalarm, s->alarmtimer);
	}

/* we only care about T2 expiry in G4. */
	if (nt && (sta == 4) && (state & V_T2_EXP)) {
		if (s->hfc_timer_on[HFC_T2])
			hfc_timer_expire(s, HFC_T2);	/* handle T2 expiry */
	}

/* If we're in F3 and receiving INFO0, start T3 and jump to F4 */
	if (!nt && (sta == 3) && (state & V_INFO0)) {
		s->hfc_timers[HFC_T3] = b4->ticks + timer_3_ms;
		s->hfc_timer_on[HFC_T3] = 1;
		if (DBG_ST) {
			dev_info(b4->dev, "port %d: receiving INFO0 in state 3, setting T3 and jumping to F4\n", s->port + 1);
		}
		hfc_force_st_state(b4, s->port, 4, 1);
	}

/* read in R_BERT_STA to determine where our current sync source is */
	newsync = b4xxp_getreg8(b4, R_BERT_STA) & 0x07;
	if (newsync != b4->syncspan) {
		if (printk_ratelimit())
			dev_info(b4->dev, "new card sync source: port %d\n", newsync + 1);
		b4->syncspan = newsync;
	}
}

/*
 * resets an S/T interface to a given NT/TE mode
 */
static void hfc_reset_st(struct b4xxp_span *s)
{
	int b;
	struct b4xxp *b4;

	b4 = s->parent;

/* force state G0/F0 (reset), then force state 1/2 (deactivated/sensing) */
	b4xxp_setreg_ra(b4, R_ST_SEL, s->port, A_ST_WR_STA, V_ST_LD_STA);
	flush_pci();			/* make sure write hit hardware */

	udelay(10);

/* set up the clock control register.  Must be done before we activate the interface. */
	if (s->te_mode)
		b = 0x0e;
	else
		b = 0x0c | (6 << V_ST_SMPL_SHIFT);

	b4xxp_setreg8(b4, A_ST_CLK_DLY, b);

/* set TE/NT mode, enable B and D channels. */
	b4xxp_setreg8(b4, A_ST_CTRL0, V_B1_EN | V_B2_EN | (s->te_mode ? 0 : V_ST_MD));
	b4xxp_setreg8(b4, A_ST_CTRL1, V_G2_G3_EN | V_E_IGNO);
	b4xxp_setreg8(b4, A_ST_CTRL2, V_B1_RX_EN | V_B2_RX_EN);

/* enable the state machine. */
	b4xxp_setreg8(b4, A_ST_WR_STA, 0x00);
	flush_pci();

	udelay(100);
}

static void hfc_start_st(struct b4xxp_span *s)
{
	struct b4xxp *b4 = s->parent;

	b4xxp_setreg_ra(b4, R_ST_SEL, s->port, A_ST_WR_STA, V_ST_ACT_ACTIVATE);

/* start T1 if in NT mode, T3 if in TE mode */
	if (s->te_mode) {
		s->hfc_timers[HFC_T3] = b4->ticks + 500;	/* 500ms wait first time, timer_t3_ms afterward. */
		s->hfc_timer_on[HFC_T3] = 1;
		s->hfc_timer_on[HFC_T1] = 0;
		if (DBG_ST)
			dev_info(b4->dev, "setting port %d t3 timer to %lu\n", s->port + 1, s->hfc_timers[HFC_T3]);
	} else {
		s->hfc_timers[HFC_T1] = b4->ticks + timer_1_ms;
		s->hfc_timer_on[HFC_T1] = 1;
		s->hfc_timer_on[HFC_T3] = 0;
		if (DBG_ST)
			dev_info(b4->dev, "setting port %d t1 timer to %lu\n", s->port + 1, s->hfc_timers[HFC_T1]);
	}
}

#if 0 /* TODO: This function is not called anywhere */
static void hfc_stop_st(struct b4xxp_span *s)
{
	b4xxp_setreg_ra(s->parent, R_ST_SEL, s->port, A_ST_WR_STA, V_ST_ACT_DEACTIVATE);

	s->hfc_timer_on[HFC_T1] = 0;
	s->hfc_timer_on[HFC_T2] = 0;
	s->hfc_timer_on[HFC_T3] = 0;
}
#endif

/*
 * read in the HFC GPIO to determine each port's mode (TE or NT).
 * Then, reset and start the port.
 * the flow controller should be set up before this is called.
 */
static void hfc_init_all_st(struct b4xxp *b4)
{
	int i, gpio, nt;
	struct b4xxp_span *s;

	gpio = b4xxp_getreg8(b4, R_GPI_IN3);

	for (i=0; i < b4->numspans; i++) {
		s = &b4->spans[i];
		s->parent = b4;
		s->port = i;

		/* The way the Digium B410P card reads the NT/TE mode
		 * jumper is the oposite of how other HFC-4S cards do:
		 * - In B410P: GPIO=0: NT
		 * - In Junghanns: GPIO=0: TE
		 */
		if (b4->card_type == B410P)
			nt = ((gpio & (1 << (i + 4))) == 0);
		else
			nt = ((gpio & (1 << (i + 4))) != 0);

		s->te_mode = !nt;

		dev_info(b4->dev, "Port %d: %s mode\n", i + 1, (nt ? "NT" : "TE"));

		hfc_reset_st(s);
		hfc_start_st(s);
	}
}

/*
 * Look at one B-channel FIFO and determine if we should exchange data with it.
 * It is assumed that the S/T port is active.
 * returns 1 if data was exchanged, 0 otherwise.
 */
static int hfc_poll_one_bchan_fifo(struct b4xxp_span *span, int c)
{
	int fifo, zlen, z1, z2, ret;
	unsigned long irq_flags;
	struct b4xxp *b4;
	struct dahdi_chan *chan;

	ret = 0;
	b4 = span->parent;
	fifo = span->fifos[c];
	chan = span->chans[c];

	spin_lock_irqsave(&b4->fifolock, irq_flags);

/* select RX FIFO */
	hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT) | V_FIFO_DIR | V_REV);

	get_Z(z1, z2, zlen);

/* TODO: error checking, full FIFO mostly */

	if (zlen >= DAHDI_CHUNKSIZE) {
		*(unsigned int *)&chan->readchunk[0] = b4xxp_getreg32(b4, A_FIFO_DATA2);
		*(unsigned int *)&chan->readchunk[4] = b4xxp_getreg32(b4, A_FIFO_DATA2);
/*
 * now TX FIFO
 *
 * Note that we won't write to the TX FIFO if there wasn't room in the RX FIFO.
 * The TX and RX sides should be kept pretty much lock-step.
 *
 * Write the last byte _NOINC so that if we don't get more data in time, we aren't leaking unknown data
 * (See HFC datasheet)
 */

		hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT) | V_REV);

		b4xxp_setreg32(b4, A_FIFO_DATA2, *(unsigned int *) &chan->writechunk[0]);
		b4xxp_setreg32(b4, A_FIFO_DATA2, *(unsigned int *) &chan->writechunk[4]);
		ret = 1;
	}

	spin_unlock_irqrestore(&b4->fifolock, irq_flags);
	return ret;
}

/*
 * Run through all of the host-facing B-channel RX FIFOs, looking for at least 8 bytes available.
 * If a B channel RX fifo has enough data, perform the data transfer in both directions.
 * D channel is done in an interrupt handler.
 * The S/T port state must be active or we ignore the fifo.
 * Returns nonzero if there was at least DAHDI_CHUNKSIZE bytes in the FIFO
 */
static int hfc_poll_fifos(struct b4xxp *b4)
{
	int ret=0, span;
	unsigned long irq_flags;

	for (span=0; span < b4->numspans; span++) {

/* Make sure DAHDI's got this span up */
		if (!(b4->spans[span].span.flags & DAHDI_FLAG_RUNNING))
			continue;

/* TODO: Make sure S/T port is in active state */
#if 0
		if (span_not_active(s))
			continue;
#endif
		ret = hfc_poll_one_bchan_fifo(&b4->spans[span], 0);
		ret |= hfc_poll_one_bchan_fifo(&b4->spans[span], 1);
	}

/* change the active FIFO one last time to make sure the last-changed FIFO updates its pointers (as per the datasheet) */
	spin_lock_irqsave(&b4->fifolock, irq_flags);
	hfc_setreg_waitbusy(b4, R_FIFO, (31 << V_FIFO_NUM_SHIFT));
	spin_unlock_irqrestore(&b4->fifolock, irq_flags);

	return ret;
}

/* NOTE: assumes fifo lock is held */
static inline void debug_fz(struct b4xxp *b4, int fifo, const char *prefix, char *buf)
{
	int f1, f2, flen, z1, z2, zlen;

	get_F(f1, f2, flen);
	get_Z(z1, z2, zlen);

	sprintf(buf, "%s: (fifo %d): f1/f2/flen=%d/%d/%d, z1/z2/zlen=%d/%d/%d\n", prefix, fifo, f1, f2, flen, z1, z2, zlen);
}

/* enable FIFO RX int and reset the FIFO */
static int hdlc_start(struct b4xxp *b4, int fifo)
{
	b4->fifo_en_txint |= (1 << fifo);
	b4->fifo_en_rxint |= (1 << fifo);

	hfc_reset_fifo_pair(b4, fifo, 1, 0);
	return 0;
}

/* disable FIFO ints and reset the FIFO */
static void hdlc_stop(struct b4xxp *b4, int fifo)
{
	b4->fifo_en_txint &= ~(1 << fifo);
	b4->fifo_en_rxint &= ~(1 << fifo);

	hfc_reset_fifo_pair(b4, fifo, 1, 0);
}

/*
 * Inner loop for D-channel receive function.
 * Retrieves a full HDLC frame from the hardware.
 * If the hardware indicates that the frame is complete,
 * we check the HDLC engine's STAT byte and update DAHDI as needed.
 *
 * Returns the number of HDLC frames left in the FIFO.
 */
static int hdlc_rx_frame(struct b4xxp_span *bspan)
{
	int fifo, i, j, zleft;
	int z1, z2, zlen, f1, f2, flen;
	unsigned char buf[WCB4XXP_HDLC_BUF_LEN];
	char debugbuf[256];
	unsigned long irq_flags;
	struct b4xxp *b4 = bspan->parent;
	unsigned char stat;

	fifo = bspan->fifos[2];

	spin_lock_irqsave(&b4->fifolock, irq_flags);
	hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT) | V_FIFO_DIR);
	get_F(f1, f2, flen);
	get_Z(z1, z2, zlen);
	debug_fz(b4, fifo, "hdlc_rx_frame", debugbuf);
	spin_unlock_irqrestore(&b4->fifolock, irq_flags);

	if (DBG_HDLC && DBG_SPANFILTER) {
		pr_info("%s", debugbuf);
	}

/* first check to make sure we really do have HDLC frames available to retrieve */
	if (flen == 0) {
		if (DBG_HDLC && DBG_SPANFILTER) {
			dev_info(b4->dev, "hdlc_rx_frame(span %d): no frames available?\n",
				bspan->port + 1);
		}

		return flen;
	}

	zleft = zlen + 1;	/* include STAT byte that the HFC injects after FCS */

	do {
		int truncated;
		if (zleft > WCB4XXP_HDLC_BUF_LEN) {
			truncated = 1;
			j = WCB4XXP_HDLC_BUF_LEN;
		} else {
			truncated = 0;
			j = zleft;
		}

		spin_lock_irqsave(&b4->fifolock, irq_flags);
		hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT) | V_FIFO_DIR);
		for (i=0; i < j; i++)
			buf[i] = b4xxp_getreg8(b4, A_FIFO_DATA0);
		spin_unlock_irqrestore(&b4->fifolock, irq_flags);

/* don't send STAT byte to DAHDI */
		if ((bspan->sigchan) && (j > 1))
			dahdi_hdlc_putbuf(bspan->sigchan, buf, truncated ? j : j - 1);

		zleft -= j;
		if (DBG_HDLC && DBG_SPANFILTER) {
			dev_info(b4->dev, "hdlc_rx_frame(span %d): z1/z2/zlen=%d/%d/%d, zleft=%d\n",
				bspan->port + 1, z1, z2, zlen, zleft);
			for (i=0; i < j; i++) printk("%02x%c", buf[i], (i < ( j - 1)) ? ' ':'\n');
		}
	} while (zleft > 0);
	stat = buf[j - 1];

/* Frame received, increment F2 and get an updated count of frames left */
	spin_lock_irqsave(&b4->fifolock, irq_flags);
	hfc_setreg_waitbusy(b4, A_INC_RES_FIFO, V_INC_F);
	get_F(f1, f2, flen);
	spin_unlock_irqrestore(&b4->fifolock, irq_flags);

	/* If this channel is not configured with a signalling span we don't
	 * need to notify the rest of dahdi about this frame. */
	if (!bspan->sigchan)
		return flen;

	++bspan->frames_in;
	if (zlen < 3) {
		if (DBG_HDLC && DBG_SPANFILTER)
			dev_notice(b4->dev, "odd, zlen less then 3?\n");
		dahdi_hdlc_abort(bspan->sigchan, DAHDI_EVENT_ABORT);
	} else {

/* if STAT != 0, indicates bad frame */
		if (stat != 0x00) {
			if (DBG_HDLC && DBG_SPANFILTER)
				dev_info(b4->dev, "(span %d) STAT=0x%02x indicates frame problem: ", bspan->port + 1, stat);
			if (stat == 0xff) {
				if (DBG_HDLC && DBG_SPANFILTER)
					printk("HDLC Abort\n");
				dahdi_hdlc_abort(bspan->sigchan, DAHDI_EVENT_ABORT);
			} else {
				if (DBG_HDLC && DBG_SPANFILTER)
					printk("Bad FCS\n");
				dahdi_hdlc_abort(bspan->sigchan, DAHDI_EVENT_BADFCS);
			}
/* STAT == 0, means frame was OK */
		} else {
			if (DBG_HDLC && DBG_SPANFILTER)
				dev_info(b4->dev, "(span %d) Frame %d is good!\n", bspan->port + 1, bspan->frames_in);
			dahdi_hdlc_finish(bspan->sigchan);
		}
	}

	return flen;
}


/*
 * Takes one blob of data from DAHDI and shoots it out to the hardware.
 * The blob may or may not be a complete HDLC frame.
 * If it isn't, the D-channel FIFO interrupt handler will take care of pulling the rest.
 * Returns nonzero if there is still data to send in the current HDLC frame.
 */
static int hdlc_tx_frame(struct b4xxp_span *bspan)
{
	struct b4xxp *b4 = bspan->parent;
	int res, i, fifo;
	int z1, z2, zlen;
	unsigned char buf[WCB4XXP_HDLC_BUF_LEN];
	unsigned int size = sizeof(buf) / sizeof(buf[0]);
	char debugbuf[256];
	unsigned long irq_flags;

/* if we're ignoring TE red alarms and we are in alarm, restart the S/T state machine */
	if (bspan->te_mode && teignorered && bspan->newalarm == DAHDI_ALARM_RED) {
		hfc_force_st_state(b4, bspan->port, 3, 1);
	}

	fifo = bspan->fifos[2];
	res = dahdi_hdlc_getbuf(bspan->sigchan, buf, &size);

	spin_lock_irqsave(&b4->fifolock, irq_flags);
	hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT));

	get_Z(z1, z2, zlen);
	debug_fz(b4, fifo, "hdlc_tx_frame", debugbuf);

/* TODO: check zlen, etc. */

	if (size > 0) {
		bspan->sigactive = 1;

		for (i=0; i < size; i++)
			b4xxp_setreg8(b4, A_FIFO_DATA0, buf[i]);

/*
 * If we got a full frame from DAHDI, increment F and decrement our HDLC pending counter.
 * Otherwise, select the FIFO again (to start transmission) and make sure the
 * TX IRQ is enabled so we will get called again to finish off the data
 */
		if (res != 0) {
			++bspan->frames_out;
			bspan->sigactive = 0;
			hfc_setreg_waitbusy(b4, A_INC_RES_FIFO, V_INC_F);
			atomic_dec(&bspan->hdlc_pending);
		} else {
			hfc_setreg_waitbusy(b4, R_FIFO, (fifo << V_FIFO_NUM_SHIFT));
			b4xxp_setreg8(b4, A_IRQ_MSK, V_IRQ);
		}
	}

/* if there are no more frames pending, disable the interrupt. */
	if (res == -1) {
		b4xxp_setreg8(b4, A_IRQ_MSK, 0);
	}

	spin_unlock_irqrestore(&b4->fifolock, irq_flags);

	if (DBG_HDLC && DBG_SPANFILTER) {
		dev_info(b4->dev, "%s", debugbuf);
		dev_info(b4->dev, "hdlc_tx_frame(span %d): DAHDI gave %d bytes for FIFO %d (res=%d)\n",
			bspan->port + 1, size, fifo, res);
		for (i=0; i < size; i++)
			printk("%02x%c", buf[i], (i < (size - 1)) ? ' ' : '\n');

		if (size && res != 0)
			pr_info("Transmitted frame %d on span %d\n", bspan->frames_out - 1, bspan->port);
	}

	return(res == 0);
}

/*
 * b4xxp lowlevel functions
 * These are functions which impact more than just the HFC controller.
 * (those are named hfc_xxx())
 */

/*
 * Performs a total reset of the card, reinitializes GPIO.
 * The card is initialized enough to have LEDs running, and that's about it.
 * Anything to do with audio and enabling any kind of processing is done in stage2.
 */
static void b4xxp_init_stage1(struct b4xxp *b4)
{
	int i;

	hfc_reset(b4);				/* total reset of controller */
	hfc_gpio_init(b4);			/* initialize controller GPIO for CPLD access */
	ec_init(b4);				/* initialize VPM and VPM GPIO */

	b4xxp_setreg8(b4, R_IRQ_CTRL, 0x00);	/* make sure interrupts are disabled */
	flush_pci();				/* make sure PCI write hits hardware */

/* disable all FIFO interrupts */
	for (i=0; i < HFC_NR_FIFOS; i++) {
		hfc_setreg_waitbusy(b4, R_FIFO, (i << V_FIFO_NUM_SHIFT));
		b4xxp_setreg8(b4, A_IRQ_MSK, 0x00);	/* disable the interrupt */
		hfc_setreg_waitbusy(b4, R_FIFO, (i << V_FIFO_NUM_SHIFT) | V_FIFO_DIR);
		b4xxp_setreg8(b4, A_IRQ_MSK, 0x00);	/* disable the interrupt */
		flush_pci();
	}

/* clear any pending FIFO interrupts */
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL0);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL1);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL2);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL3);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL4);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL5);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL6);
	b4xxp_getreg8(b4, R_IRQ_FIFO_BL7);

	b4xxp_setreg8(b4, R_SCI_MSK, 0x00);	/* mask off all S/T interrupts */
	b4xxp_setreg8(b4, R_IRQMSK_MISC, 0x00);	/* nothing else can generate an interrupt */

	/*
	 * set up the clock controller B410P & Cologne Eval Board have a
	 * 24.576MHz crystal, so the PCM clock is 2x the incoming clock.
	 * Other cards have a 49.152Mhz crystal, so the PCM clock equals
	 * incoming clock.
	 */

	if ((b4->card_type == B410P) || (b4->card_type == QUADBRI_EVAL))
		b4xxp_setreg8(b4, R_BRG_PCM_CFG, 0x02);
	else
		b4xxp_setreg8(b4, R_BRG_PCM_CFG, V_PCM_CLK);

	flush_pci();

	udelay(100);				/* wait a bit for clock to settle */
}

/*
 * Stage 2 hardware init.
 * Sets up the flow controller, PCM and FIFOs.
 * Initializes the echo cancellers.
 * S/T interfaces are not initialized here, that is done later, in hfc_init_all_st().
 * Interrupts are enabled and once the s/t interfaces are configured, chip should be pretty much operational.
 */
static void b4xxp_init_stage2(struct b4xxp *b4)
{
	int span;

/*
 * set up PCM bus.
 * HFC is PCM master.
 * C4IO, SYNC_I and SYNC_O unused.
 * 32 channels, frame signal positive polarity, active for 2 C4 clocks.
 * only the first two timeslots in each quad are active
 * STIO0 is transmit-only, STIO1 is receive-only.
 */
	b4xxp_setreg8(b4, R_PCM_MD0, V_PCM_MD | V_PCM_IDX_MD1);
	flush_pci();
	b4xxp_setreg8(b4, R_PCM_MD1, V_PLL_ADJ_00 | V_PCM_DR_2048);

	b4xxp_setreg8(b4, R_PWM_MD, 0xa0);
	b4xxp_setreg8(b4, R_PWM0, 0x1b);

/*
 * set up the flow controller.
 * B channel map: (4 ports cards with Hardware Echo Cancel present & active)
 * FIFO 0 connects Port 1 B0 using HFC channel 16 and PCM timeslots 0/1.
 * FIFO 1 connects Port 1 B1 using HFC channel 17 and PCM timeslots 4/5.
 * FIFO 2 connects Port 2 B0 using HFC channel 20 and PCM timeslots 8/9.
 * FIFO 3 connects Port 2 B1 using HFC channel 21 and PCM timeslots 12/13.
 * FIFO 4 connects Port 3 B0 using HFC channel 24 and PCM timeslots 16/17.
 * FIFO 5 connects Port 3 B1 using HFC channel 25 and PCM timeslots 20/21.
 * FIFO 6 connects Port 4 B0 using HFC channel 28 and PCM timeslots 24/25.
 * FIFO 7 connects Port 4 B1 using HFC channel 29 and PCM timeslots 28/29.
 *
 * All B channel FIFOs have their HDLC controller in transparent mode,
 * and only the FIFO for B0 on each port has its interrupt operational.
 *
 * D channels are handled by FIFOs 8-11.
 * FIFO 8 connects Port 1 D using HFC channel 3
 * FIFO 9 connects Port 2 D using HFC channel 7
 * FIFO 10 connects Port 3 D using HFC channel 11
 * FIFO 11 connects Port 4 D using HFC channel 15
 *
 * D channel FIFOs are operated in HDLC mode and interrupt on end of frame.
 *
 * B channel map: (8 ports cards without Hardware Echo Cancel)
 * FIFO 0 connects Port 1 B0 using HFC channel 0
 * FIFO 1 connects Port 1 B1 using HFC channel 1
 * FIFO 2 connects Port 2 B0 using HFC channel 4
 * FIFO 3 connects Port 2 B1 using HFC channel 5
 * .........................
 * FIFO 14 connects Port 8 B0 using HFC channel 28
 * FIFO 15 connects Port 8 B1 using HFC channel 29
 *
 * All B channel FIFOs have their HDLC controller in transparent mode,
 * and only the FIFO for B0 on each port has its interrupt operational.
 *
 * D channels are handled by FIFOs 16-23.
 * FIFO 16 connects Port 1 D using HFC channel 3
 * FIFO 17 connects Port 2 D using HFC channel 7
 * FIFO 18 connects Port 3 D using HFC channel 11
 * FIFO 19 connects Port 4 D using HFC channel 15
 * ................
 * FIFO 23 connects Port 8 D using HFC channel 31
 * D channel FIFOs are operated in HDLC mode and interrupt on end of frame.
 */
	for (span=0; span < b4->numspans; span++) {
		if ((vpmsupport) && (CARD_HAS_EC(b4))) {
			hfc_assign_bchan_fifo_ec(b4, span, 0);
			hfc_assign_bchan_fifo_ec(b4, span, 1);
		} else {
			hfc_assign_bchan_fifo_noec(b4, span, 0);
			hfc_assign_bchan_fifo_noec(b4, span, 1);
		}
		hfc_assign_dchan_fifo(b4, span);
	}

/* set up the timer interrupt for 1ms intervals */
	b4xxp_setreg8(b4, R_TI_WD, (2 << V_EV_TS_SHIFT));

/*
 * At this point, everything's set up and ready to go.
 * Don't actually enable the global interrupt pin.
 * DAHDI still needs to start up the spans, and we don't know exactly when.
 */
}

static void b4xxp_setleds(struct b4xxp *b4, unsigned char val)
{
	ec_write(b4, 0, 0x1a8 + 3, val);
}

static void b4xxp_update_leds_hfc_8s(struct b4xxp *b4)
{
	unsigned long lled = 0; /* A bit set is a led OFF */
	unsigned long leddw;
	int j;
	struct b4xxp_span *bspan;

	b4->blinktimer++;
	for (j = 7; j >= 0; j--) {
		bspan = &b4->spans[7 - j];
		if (!(bspan->span.flags & DAHDI_FLAG_RUNNING) ||
				bspan->span.alarms) {
			BIT_SET(lled, j);
			continue;  /* Led OFF */
		}

		if (bspan->span.mainttimer || bspan->span.maintstat) {
			/* Led Blinking in maint state */
			if (b4->blinktimer >= 0x7f)
				BIT_SET(lled, j);
		}
		/* Else: Led on */
	}

	/* Write Leds...*/
	leddw = lled << 24 | lled << 16 | lled << 8 | lled;
	b4xxp_setreg8(b4, R_BRG_PCM_CFG, 0x21);
	iowrite16(0x4000, b4->ioaddr + 4);
	iowrite32(leddw, b4->ioaddr);
	b4xxp_setreg8(b4, R_BRG_PCM_CFG, 0x20);

	if (b4->blinktimer == 0xff)
		b4->blinktimer = -1;
}

/* So far only tested for OpenVox cards. Please test it for other hardware */
static void b4xxp_update_leds_hfc(struct b4xxp *b4)
{
	int i;
	int leds = 0, green_leds = 0; /* Default: off */
	struct b4xxp_span *bspan;

	b4->blinktimer++;
	for (i=0; i < b4->numspans; i++) {
		bspan = &b4->spans[i];

		if (!(bspan->span.flags & DAHDI_FLAG_RUNNING))
			continue; /* Leds are off */

		if (bspan->span.alarms) {
			/* Red blinking -> Alarm */
			if (b4->blinktimer >= 0x7f)
				BIT_SET(leds, i);
		} else if (bspan->span.mainttimer || bspan->span.maintstat) {
			/* Green blinking -> Maint status */
			if (b4->blinktimer >= 0x7f)
				BIT_SET(green_leds, i);
		} else {
			/* Steady grean -> No Alarm */
			BIT_SET(green_leds, i);
		}
	}

	/* Actually set them. for red: just set the bit in R_GPIO_EN1.
	   For green: in both R_GPIO_EN1 and R_GPIO_OUT1. */
	leds |= green_leds;
	b4xxp_setreg8(b4, R_GPIO_EN1, leds);
	b4xxp_setreg8(b4, R_GPIO_OUT1, green_leds);

	if (b4->blinktimer == 0xff)
		b4->blinktimer = -1;
}

static void b4xxp_set_span_led(struct b4xxp *b4, int span, unsigned char val)
{
	int shift, spanmask;

	shift = span << 1;
	spanmask = ~(0x03 << shift);

	b4->ledreg &= spanmask;
	b4->ledreg |= (val << shift);
	b4xxp_setleds(b4, b4->ledreg);
}

static void b4xxp_update_leds(struct b4xxp *b4)
{
	int i;
	struct b4xxp_span *bspan;

	if (b4->numspans == 8) {
		/* Use the alternative function for non-Digium HFC-8S cards */
		b4xxp_update_leds_hfc_8s(b4);
		return;
	}

	if (b4->card_type != B410P) {
		/* Use the alternative function for non-Digium HFC-4S cards */
		b4xxp_update_leds_hfc(b4);
		return;
	}

	b4->blinktimer++;
	for (i=0; i < b4->numspans; i++) {
		bspan = &b4->spans[i];

		if (bspan->span.flags & DAHDI_FLAG_RUNNING) {
			if (bspan->span.alarms) {
				if (b4->blinktimer == (led_fader_table[b4->alarmpos] >> 1))
					b4xxp_set_span_led(b4, i, LED_RED);
				if (b4->blinktimer == 0xf)
					b4xxp_set_span_led(b4, i, LED_OFF);
			} else if (bspan->span.mainttimer || bspan->span.maintstat) {
				if (b4->blinktimer == (led_fader_table[b4->alarmpos] >> 1))
					b4xxp_set_span_led(b4, i, LED_GREEN);
				if (b4->blinktimer == 0xf)
					b4xxp_set_span_led(b4, i, LED_OFF);
			} else {
				/* No Alarm */
				b4xxp_set_span_led(b4, i, LED_GREEN);
			}
		}	else
				b4xxp_set_span_led(b4, i, LED_OFF);
	}

	if (b4->blinktimer == 0xf) {
		b4->blinktimer = -1;
		b4->alarmpos++;
		if (b4->alarmpos >= (sizeof(led_fader_table) / sizeof(led_fader_table[0])))
			b4->alarmpos = 0;
	}
}

static int echocan_create(struct dahdi_chan *chan, struct dahdi_echocanparams *ecp,
			  struct dahdi_echocanparam *p, struct dahdi_echocan_state **ec)
{
	struct b4xxp_span *bspan = chan->span->pvt;
	int channel;

	if (chan->chanpos == 3) {
		printk(KERN_WARNING "Cannot enable echo canceller on D channel of span %d; failing request\n", chan->span->offset);
		return -EINVAL;
	}

	if (ecp->param_count > 0) {
		printk(KERN_WARNING "wcb4xxp echo canceller does not support parameters; failing request\n");
		return -EINVAL;
	}

	*ec = &bspan->ec[chan->chanpos];
	(*ec)->ops = &my_ec_ops;
	(*ec)->features = my_ec_features;

	if (DBG_EC)
		printk("Enabling echo cancellation on chan %d span %d\n", chan->chanpos, chan->span->offset);

	channel = (chan->span->offset * 8) + ((chan->chanpos - 1) * 4) + 1;
	
	ec_write(bspan->parent, chan->chanpos - 1, channel, 0x7e);

	return 0;
}

static void echocan_free(struct dahdi_chan *chan, struct dahdi_echocan_state *ec)
{
	struct b4xxp_span *bspan = chan->span->pvt;
	int channel;

	memset(ec, 0, sizeof(*ec));

	if (DBG_EC)
		printk("Disabling echo cancellation on chan %d span %d\n", chan->chanpos, chan->span->offset);

	channel = (chan->span->offset * 8) + ((chan->chanpos - 1) * 4) + 1;

	ec_write(bspan->parent, chan->chanpos - 1, channel, 0x01);
}

/*
 * Filesystem and DAHDI interfaces
 */
static int b4xxp_ioctl(struct dahdi_chan *chan, unsigned int cmd, unsigned long data)
{
	switch(cmd) {
	default:
		return -ENOTTY;
	}

	return 0;
}

static int b4xxp_startup(struct dahdi_span *span)
{
	struct b4xxp_span *bspan = span->pvt;
	struct b4xxp *b4 = bspan->parent;

	if (!b4->running)
		hfc_enable_interrupts(bspan->parent);

	return 0;
}

static int b4xxp_shutdown(struct dahdi_span *span)
{
	struct b4xxp_span *bspan = span->pvt;

	hfc_disable_interrupts(bspan->parent);
	return 0;
}

/* resets all the FIFOs for a given span. Disables IRQs for the span FIFOs */
static void b4xxp_reset_span(struct b4xxp_span *bspan)
{
	int i;
	struct b4xxp *b4 = bspan->parent;

	for (i=0; i < 3; i++) {
		hfc_reset_fifo_pair(b4, bspan->fifos[i], (i == 2) ? 1 : 0, 1);
	}

	b4xxp_set_sync_src(b4, b4xxp_find_sync(b4));
}

/* spanconfig for us means to set up the HFC FIFO and channel mapping */
static int b4xxp_spanconfig(struct dahdi_span *span, struct dahdi_lineconfig *lc)
{
	int i;
	struct b4xxp_span *bspan = span->pvt;
	struct b4xxp *b4 = bspan->parent;

	if (DBG)
		dev_info(b4->dev, "Configuring span %d\n", span->spanno);

#if 0
	if (lc->sync > 0 && bspan->te_mode) {
		dev_info(b4->dev, "Span %d is not in NT mode, removing from sync source list\n", span->spanno);
		lc->sync = 0;
	}
#endif
	if (lc->sync < 0 || lc->sync > 4) {
		dev_info(b4->dev, "Span %d has invalid sync priority (%d), removing from sync source list\n", span->spanno, lc->sync);
		lc->sync = 0;
	}

	/* remove this span number from the current sync sources, if there */
	for (i = 0; i < b4->numspans; i++) {
		if (b4->spans[i].sync == span->spanno) {
			b4->spans[i].sync = 0;
		}
	}

	/* if a sync src, put it in proper place */
	b4->spans[span->offset].syncpos = lc->sync;
	if (lc->sync) {
		b4->spans[lc->sync - 1].sync = span->spanno;
	}

	b4xxp_reset_span(bspan);

/* call startup() manually here, because DAHDI won't call the startup function unless it receives an IOCTL to do so, and dahdi_cfg doesn't. */
	b4xxp_startup(&bspan->span);

	span->flags |= DAHDI_FLAG_RUNNING;

	return 0;
}

/* chanconfig for us means to configure the HDLC controller, if appropriate */
static int b4xxp_chanconfig(struct dahdi_chan *chan, int sigtype)
{
	int alreadyrunning;
	struct b4xxp *b4 = chan->pvt;
	struct b4xxp_span *bspan = &b4->spans[chan->span->offset];
	int fifo = bspan->fifos[2];

	alreadyrunning = bspan->span.flags & DAHDI_FLAG_RUNNING;

	if (DBG_FOPS) {
		dev_info(b4->dev, "%s channel %d (%s) sigtype %08x\n",
			alreadyrunning ? "Reconfigured" : "Configured", chan->channo, chan->name, sigtype);
	}

	/* (re)configure signalling channel */
	if ((sigtype == DAHDI_SIG_HARDHDLC) || (bspan->sigchan == chan)) {
		if (DBG_FOPS)
			dev_info(b4->dev, "%sonfiguring hardware HDLC on %s\n",
				((sigtype == DAHDI_SIG_HARDHDLC) ? "C" : "Unc"), chan->name);

		if (alreadyrunning && bspan->sigchan) {
			hdlc_stop(b4, fifo);
			bspan->sigchan = NULL;
		}

		if (sigtype == DAHDI_SIG_HARDHDLC) {
			if (hdlc_start(b4, fifo)) {
				dev_warn(b4->dev, "Error initializing signalling controller\n");
				return -1;
			}
		}

		bspan->sigchan = (sigtype == DAHDI_SIG_HARDHDLC) ? chan : NULL;
		bspan->sigactive = 0;
		atomic_set(&bspan->hdlc_pending, 0);
	} else {
/* FIXME: shouldn't I be returning an error? */
	}

	return 0;
}

static int b4xxp_open(struct dahdi_chan *chan)
{
	struct b4xxp *b4 = chan->pvt;
	struct b4xxp_span *bspan = &b4->spans[chan->span->offset];

	if (DBG_FOPS && DBG_SPANFILTER)
		dev_info(b4->dev, "open() on chan %s (%i/%i)\n", chan->name, chan->channo, chan->chanpos);

	hfc_reset_fifo_pair(b4, bspan->fifos[chan->chanpos], 0, 0);
	return 0;
}


static int b4xxp_close(struct dahdi_chan *chan)
{
	struct b4xxp *b4 = chan->pvt;
	struct b4xxp_span *bspan = &b4->spans[chan->span->offset];

	if (DBG_FOPS && DBG_SPANFILTER)
		dev_info(b4->dev, "close() on chan %s (%i/%i)\n", chan->name, chan->channo, chan->chanpos);

	hfc_reset_fifo_pair(b4, bspan->fifos[chan->chanpos], 1, 1);
	return 0;
}

/* DAHDI calls this when it has data it wants to send to the HDLC controller */
static void b4xxp_hdlc_hard_xmit(struct dahdi_chan *chan)
{
	struct b4xxp *b4 = chan->pvt;
	int span = chan->span->offset;
	struct b4xxp_span *bspan = &b4->spans[span];

	if ((DBG_FOPS || DBG_HDLC) && DBG_SPANFILTER)
		dev_info(b4->dev, "hdlc_hard_xmit on chan %s (%i/%i), span=%i\n",
			chan->name, chan->channo, chan->chanpos, span + 1);

/*
 * increment the hdlc_pending counter and trigger the bottom-half so it
 * will be picked up and sent.
 */
	if (bspan->sigchan == chan) {
		atomic_inc(&bspan->hdlc_pending);
	}
}

/* internal functions, not specific to the hardware or DAHDI */

/* initialize the span/chan structures. Doesn't touch hardware, although the callbacks might. */
static void init_spans(struct b4xxp *b4)
{
	int i, j;
	struct b4xxp_span *bspan;
	struct dahdi_chan *chan;

/* for each span on the card */
	for (i=0; i < b4->numspans; i++) {
		bspan = &b4->spans[i];
		bspan->parent = b4;

		bspan->span.irq = b4->pdev->irq;
		bspan->span.pvt = bspan;
		bspan->span.spantype = (bspan->te_mode) ? "TE" : "NT";
		bspan->span.offset = i;
		bspan->span.channels = WCB4XXP_CHANNELS_PER_SPAN;
		bspan->span.flags = 0;

		if (alawoverride)	
			bspan->span.deflaw = DAHDI_LAW_ALAW;
		else
			bspan->span.deflaw = DAHDI_LAW_MULAW;
		/* For simplicty, we'll accept all line modes since BRI
		 * ignores this setting anyway.*/
		bspan->span.linecompat = DAHDI_CONFIG_AMI | 
			DAHDI_CONFIG_B8ZS | DAHDI_CONFIG_D4 | 
			DAHDI_CONFIG_ESF | DAHDI_CONFIG_HDB3 |
			DAHDI_CONFIG_CCS | DAHDI_CONFIG_CRC4;

		sprintf(bspan->span.name, "B4/%d/%d", b4->cardno, i+1);
		sprintf(bspan->span.desc, "B4XXP (PCI) Card %d Span %d", b4->cardno, i+1);
		bspan->span.manufacturer = "Digium";
		dahdi_copy_string(bspan->span.devicetype, b4->variety, sizeof(bspan->span.devicetype));
		sprintf(bspan->span.location, "PCI Bus %02d Slot %02d",
			b4->pdev->bus->number, PCI_SLOT(b4->pdev->devfn) + 1);

		bspan->span.owner = THIS_MODULE;
		bspan->span.spanconfig = b4xxp_spanconfig;
		bspan->span.chanconfig = b4xxp_chanconfig;
		bspan->span.startup = b4xxp_startup;
		bspan->span.shutdown = b4xxp_shutdown;
		bspan->span.open = b4xxp_open;
		bspan->span.close  = b4xxp_close;
		bspan->span.ioctl = b4xxp_ioctl;
		bspan->span.hdlc_hard_xmit = b4xxp_hdlc_hard_xmit;
		if (vpmsupport && CARD_HAS_EC(b4))
			bspan->span.echocan_create = echocan_create;

/* HDLC stuff */
		bspan->sigchan = NULL;
		bspan->sigactive = 0;

		bspan->span.chans = bspan->chans;
		init_waitqueue_head(&bspan->span.maintq);

/* now initialize each channel in the span */
		for (j=0; j < WCB4XXP_CHANNELS_PER_SPAN; j++) {
			bspan->chans[j] = &bspan->_chans[j];
			chan = bspan->chans[j];
			chan->pvt = b4;

			sprintf(chan->name, "B4/%d/%d/%d", b4->cardno, i + 1, j + 1);
			/* The last channel in the span is the D-channel */
			if (j == WCB4XXP_CHANNELS_PER_SPAN - 1) {
				chan->sigcap = DAHDI_SIG_HARDHDLC;
			} else {
				chan->sigcap = DAHDI_SIG_CLEAR | DAHDI_SIG_DACS;
			}
			chan->chanpos = j + 1;
			chan->writechunk = (void *)(bspan->writechunk + j * DAHDI_CHUNKSIZE);
			chan->readchunk = (void *)(bspan->readchunk + j * DAHDI_CHUNKSIZE);
		}
	}
}


static void b4xxp_bottom_half(unsigned long data);

/* top-half interrupt handler */
DAHDI_IRQ_HANDLER(b4xxp_interrupt)
{
	struct b4xxp *b4 = dev_id;
	unsigned char status;
	int i;

	/* Make sure it's really for us */
	status = __pci_in8(b4, R_STATUS);
	if (!(status & HFC_INTS))
		return IRQ_NONE;

/*
 * since the interrupt is for us, read in the FIFO and misc IRQ status registers.
 * Don't replace the struct copies; OR in the new bits instead.
 * That way if we get behind, we don't lose anything.
 * We don't actually do any processing here, we simply flag the bottom-half to do the heavy lifting.
 */
	if (status & V_FR_IRQSTA) {
		b4->fifo_irqstatus[0] |= __pci_in8(b4, R_IRQ_FIFO_BL0);
		b4->fifo_irqstatus[1] |= __pci_in8(b4, R_IRQ_FIFO_BL1);
		b4->fifo_irqstatus[2] |= __pci_in8(b4, R_IRQ_FIFO_BL2);
		b4->fifo_irqstatus[3] |= __pci_in8(b4, R_IRQ_FIFO_BL3);
		b4->fifo_irqstatus[4] |= __pci_in8(b4, R_IRQ_FIFO_BL4);
		b4->fifo_irqstatus[5] |= __pci_in8(b4, R_IRQ_FIFO_BL5);
		b4->fifo_irqstatus[6] |= __pci_in8(b4, R_IRQ_FIFO_BL6);
		b4->fifo_irqstatus[7] |= __pci_in8(b4, R_IRQ_FIFO_BL7);
	}

	if (status & V_MISC_IRQSTA) {
		b4->misc_irqstatus |= __pci_in8(b4, R_IRQ_MISC);
	}

/*
 * Well, that was the plan.  It appears that I can't do this in the bottom half
 * or I start to see data corruption (too long a time between IRQ and tasklet??)
 * So, I do the B-channel stuff right here in interrupt context.  yuck.
 */
	if (b4->misc_irqstatus & V_TI_IRQ) {
		hfc_poll_fifos(b4);
		for (i=0; i < b4->numspans; i++) {
			if (b4->spans[i].span.flags & DAHDI_FLAG_RUNNING) {
				dahdi_ec_span(&b4->spans[i].span);
				dahdi_receive(&b4->spans[i].span);
				dahdi_transmit(&b4->spans[i].span);
			}
		}
	}

/* kick off bottom-half handler */
	/* tasklet_hi_schedule(&b4->b4xxp_tlet); */
	b4xxp_bottom_half((unsigned long)b4);

	return IRQ_RETVAL(1);
}


/*
 * The bottom half of course does all the heavy lifting for the interrupt.
 *
 * The original plan was to have the B channel RX FIFO interrupts enabled, and
 * to do the actual work here.  Since that doesn't seem to work so well, we
 * poll the B channel FIFOs right in the interrupt handler and take care of the B
 * channel stuff there.  The bottom half works for the timer interrupt and D
 * channel stuff.
 *
 * The HFC-4S timer interrupt is used to for several things:
 * - Update the S/T state machines, expire their timers, etc.
 * - Provide DAHDI's timing source, if so configured
 * - Update LEDs
 */
static void b4xxp_bottom_half(unsigned long data)
{
	struct b4xxp *b4 = (struct b4xxp *)data;
	int i, j, k, gotrxfifo, fifo, fifo_low, fifo_high;
	unsigned char b, b2;

	if (b4->shutdown)
		return;

	gotrxfifo = 0;
	/* HFC-4S d-chan fifos 8-11 *** HFC-8S d-chan fifos 16-23 */
	if (b4->numspans == 8) {
		fifo_low = 16;
		fifo_high = 23;
	} else {
		fifo_low = 8;
		fifo_high = 11;
	}

	for (i=0; i < 8; i++) {
		b = b2 = b4->fifo_irqstatus[i];

		for (j=0; j < b4->numspans; j++) {
			fifo = i*4 + j;

			if (b & V_IRQ_FIFOx_TX) {
				if (fifo >= fifo_low && fifo <= fifo_high) {
					/* d-chan fifos */
/*
 * WOW I don't like this.
 * It's bad enough that I have to send a fake frame to get an HDLC TX FIFO interrupt,
 * but now, I have to loop until the whole frame is read, or I get RX interrupts
 * (even though the chip says HDLC mode gives an IRQ when a *full frame* is received).
 * Yuck.  It works well, but yuck.
 */
					do {
						k = hdlc_tx_frame(&b4->spans[fifo - fifo_low]);
					}  while (k);
				} else {
					if (printk_ratelimit())
						dev_warn(b4->dev, "Got FIFO TX int from non-d-chan FIFO %d??\n", fifo);
				}
			}

			if (b & V_IRQ_FIFOx_RX) {
				if (fifo >= fifo_low && fifo <= fifo_high) {	/* dchan fifos */
/*
 * I have to loop here until hdlc_rx_frame says there are no more frames waiting.
 * for whatever reason, the HFC will not generate another interrupt if there are
 * still HDLC frames waiting to be received.
 * i.e. I get an int when F1 changes, not when F1 != F2.
 */
					do {
						k = hdlc_rx_frame(&b4->spans[fifo - fifo_low]);
					} while (k);
				} else {
					if (printk_ratelimit())
						dev_warn(b4->dev, "Got FIFO RX int from non-d-chan FIFO %d??\n", fifo);
				}
			}

			b >>= 2;
		}

/* zero the bits we just processed */
		b4->fifo_irqstatus[i] &= ~b2;
	}

/*
 * timer interrupt
 * every tick (1ms), check the FIFOs and run through the S/T port timers.
 * every 100ms or so, look for S/T state machine changes.
 */
	if (b4->misc_irqstatus & V_TI_IRQ) {

/*
 * We should check the FIFOs here, but I'm seeing this tasklet getting scheduled FAR too late to be useful.
 * For now, we're handling that in the IRQ handler itself.  (ICK!!)
 */
		b4->ticks++;

		hfc_update_st_timers(b4);

		b4xxp_update_leds(b4);

/* every 100ms or so, look at the S/T interfaces to see if they changed state */
		if (!(b4->ticks % 100)) {
			b = b4xxp_getreg8(b4, R_SCI);
			if (b) {
				for (i=0; i < b4->numspans; i++) {
					if (b & (1 << i))
						hfc_handle_state(&b4->spans[i]);
				}
			}
		}

/* We're supposed to kick DAHDI here, too, but again, seeing too much latency between the interrupt and the bottom-half. */

/* clear the timer interrupt flag. */
		b4->misc_irqstatus &= ~V_TI_IRQ;
	}

/*
 * Check for outgoing HDLC frame requests
 * The HFC does not generate TX interrupts when there is room to send, so
 * I use an atomic counter that is incremented every time DAHDI wants to send
 * a frame, and decremented every time I send a frame.  It'd be better if I could
 * just use the interrupt handler, but the HFC seems to trigger a FIFO TX IRQ 
 * only when it has finished sending a frame, not when one can be sent.
 */
	for (i=0; i < b4->numspans; i++) {
		struct b4xxp_span *bspan = &b4->spans[i];

		if (atomic_read(&bspan->hdlc_pending)) {
			do {
				k = hdlc_tx_frame(bspan);
			}  while (k);
                }
        }
}


/********************************************************************************* proc stuff *****/

#ifdef CREATE_WCB4XXP_PROCFS_ENTRY
static int b4xxp_proc_read_one(char *buf, struct b4xxp *b4)
{
	struct dahdi_chan *chan;
	int len, i, j;
	char str[80], sBuf[4096];

	*sBuf=0;
	sprintf(sBuf, "Card %d, PCI identifier %s, IRQ %d\n", b4->cardno + 1, b4->dev->bus_id, b4->irq);

	strcat(sBuf,"Tx:\n");
	for (j=0; j<(b4->numspans * 2) ; j++) {			/* B Channels */
		for (i=0; i<(b4->numspans * 3) ; i++) {		/* All Channels */
			chan = b4->spans[i/3].chans[i%3];
			sprintf(str, "%02x ", chan->writechunk[j]);
			strcat(sBuf, str);
		}

		strcat(sBuf, "\n");
	}

	strcat(sBuf, "\nRx:\n");
	for (j=0; j < (b4->numspans * 2); j++) {		/* B Channels */
		for (i=0; i < (b4->numspans * 3); i++) { 	/* All Channels */
			chan = b4->spans[i / 3].chans[i % 3];
			sprintf(str, "%02x%c", chan->readchunk[j], (i == 11) ? '\n' : ' ');
			strcat(sBuf, str);
		}
	}

	strcat(sBuf, "\nPort states:\n");
	for (i=0; i < b4->numspans; i++) {
		int state;
		char *x;
		struct b4xxp_span *s = &b4->spans[i];

		state = b4xxp_getreg_ra(b4, R_ST_SEL, s->port, A_ST_RD_STA);
		x = hfc_decode_st_state(b4, s->port, state, 0);
		sprintf(str, "%s\n", x);
		strcat(sBuf, str);
		kfree(x);
	}

	len = sprintf(buf, "%s\n%s\nTicks: %ld\n", sBuf, str, b4->ticks);
	return len;
}

static int b4xxp_proc_read(char *buf, char **start, off_t offset, int count, int *eof, void *data)
{
	struct b4xxp **b4_cards = data;
	char sBuf[256];
	int i, len;

	len = sprintf(buf, "WCB4XXP Card Information\n");
	for (i=0; b4_cards[i] != NULL; i++) {
		if (i)
			len += sprintf(buf + len, "\n-----\n");
		len += b4xxp_proc_read_one(buf + len, b4_cards[i]);
	}

	*sBuf = 0;
	strcat(sBuf, "\n-----\n\nAudio: ");
#ifdef LOOPBACK_SUPPORTED
	if (loopback >= 3)
		strcat(sBuf, "DAHDI and S/T");
	else if (loopback == 2)
		strcat(sBuf, "DAHDI");
	else if (loopback == 1)
		strcat(sBuf, "S/T");
	else
		strcat(sBuf, "not");
	strcat(sBuf, " looped back");
#else
	strcat(sBuf, "not looped back");
#endif

	if (milliwatt)
		strcat(sBuf, ", outgoing S/T replaced with mu-law milliwatt tone");

	len += sprintf(buf + len, "%s\n", sBuf);

	if (alarmdebounce)
		sprintf(sBuf, "Alarms: debounced (%dms)", alarmdebounce);
	else
		strcpy(sBuf, "Alarms: not debounced");

	len += sprintf(buf + len, "%s\nT1 timer period %dms\nT3 timer period %dms\n", sBuf, timer_1_ms, timer_3_ms);

	*eof = 1;
	return len;
}
#endif /* CREATE_WCB4XXP_PROCFS_ENTRY */

static int b4xxp_startdefaultspan(struct b4xxp *b4)
{
	struct dahdi_lineconfig lc = {0,};
	return b4xxp_spanconfig(&b4->spans[0].span, &lc);
}

static int __devinit b4xx_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int x, ret;
	struct b4xxp *b4;
	struct devtype *dt;

	dt = (struct devtype *)(ent->driver_data);
	dev_info(&pdev->dev, "probe called for b4xx...\n");

	if ((ret = pci_enable_device(pdev)))
		goto err_out_disable_pdev;

	if ((ret = pci_request_regions(pdev, dt->desc))) {
		dev_err(&pdev->dev, "Unable to request regions!\n");
		goto err_out_disable_pdev;
	}

	if (!pdev->irq) {			/* we better have an IRQ */
		dev_err(&pdev->dev, "Device has no associated IRQ?\n");
		ret = -EIO;
		goto err_out_release_regions;
	}

	if (!(b4 = kzalloc(sizeof(struct b4xxp), GFP_KERNEL))) {
		dev_err(&pdev->dev, "Couldn't allocate memory for b4xxp structure!\n");
		ret = -ENOMEM;
		goto err_out_release_regions;
	}

/* card found, enabled and main struct allocated.  Fill it out. */
	b4->variety = dt->desc;
	b4->card_type = dt->card_type;
	b4->pdev = pdev;
	b4->dev = &pdev->dev;
	pci_set_drvdata(pdev, b4);

	b4->ioaddr = pci_iomap(pdev, 0, 0);
	b4->addr = pci_iomap(pdev, 1, 0);
	b4->irq = pdev->irq;

	spin_lock_init(&b4->reglock);
	spin_lock_init(&b4->seqlock);
	spin_lock_init(&b4->fifolock);

	x = b4xxp_getreg8(b4, R_CHIP_ID);
	if ((x != 0xc0) && (x != 0x80)) {		/* wrong chip? */
		dev_err(&pdev->dev, "Unknown/unsupported controller detected (R_CHIP_ID = 0x%02x)\n", x);
		goto err_out_free_mem;
	}

/* future proofing */
	b4->chiprev = b4xxp_getreg8(b4, R_CHIP_RV);

/* check for various board-specific flags and modify init as necessary */
/*
	if (dt->flags & FLAG_XXX)
		use_flag_somehow();
*/

/* TODO: determine whether this is a 2, 4 or 8 port card */
	b4->numspans = dt->ports;
	b4->syncspan = -1;		/* sync span is unknown */
	if (b4->numspans > MAX_SPANS_PER_CARD) {
		dev_err(b4->dev, "Driver does not know how to handle a %d span card!\n", b4->numspans);
		goto err_out_free_mem;
	}

	dev_info(b4->dev, "Identified %s (controller rev %d) at %p, IRQ %i\n",
		b4->variety, b4->chiprev, b4->ioaddr, b4->irq);

/* look for the next free card structure */
	for (x=0; x < MAX_B4_CARDS; x++) {
		if (!cards[x])
			break;
	}

	if (x >= MAX_B4_CARDS) {
		dev_err(&pdev->dev, "Attempt to register more than %i cards, aborting!\n", MAX_B4_CARDS);
		goto err_out_free_mem;
	}

/* update the cards array, make sure the b4xxp struct knows where in the array it is */
	b4->cardno = x;
	cards[x] = b4;

	b4xxp_init_stage1(b4);

	if (request_irq(pdev->irq, b4xxp_interrupt, DAHDI_IRQ_SHARED_DISABLED, "b4xxp", b4)) {
		dev_err(b4->dev, "Unable to request IRQ %d\n", pdev->irq);
		ret = -EIO;
		goto err_out_del_from_card_array;
	}

/* initialize the tasklet structure */
/* TODO: perhaps only one tasklet for any number of cards in the system... don't need one per card I don't think. */
	tasklet_init(&b4->b4xxp_tlet, b4xxp_bottom_half, (unsigned long)b4);

/* interrupt allocated and tasklet initialized, it's now safe to finish initializing the hardware */
	b4xxp_init_stage2(b4);
	hfc_init_all_st(b4);

/* initialize the DAHDI structures, and let DAHDI know it has some new hardware to play with */
	init_spans(b4);
	for (x=0; x < b4->numspans; x++) {
		if (dahdi_register(&b4->spans[x].span, 0)) {
			dev_err(b4->dev, "Unable to register span %s\n", b4->spans[x].span.name);
			goto err_out_unreg_spans;
		}
	}



#if 0
	/* Launch cards as appropriate */
	for (;;) {
		/* Find a card to activate */
		f = 0;
		for (x=0; cards[x]; x++) {
			if (cards[x]->order <= highestorder) {
				b4_launch(cards[x]);
				if (cards[x]->order == highestorder)
					f = 1;
			}
		}
		/* If we found at least one, increment the highest order and search again, otherwise stop */
		if (f)
			highestorder++;
		else
			break;
	}
#else
	dev_info(b4->dev, "Did not do the highestorder stuff\n");
#endif

	ret = b4xxp_startdefaultspan(b4);
	if (ret)
		goto err_out_unreg_spans;

	ret = 0;
	return ret;

/* 'x' will have the failing span #. (0-3).  We need to unregister everything before it. */
err_out_unreg_spans:
	while (x) {
		dahdi_unregister(&b4->spans[x].span);
		x--;
	};

	b4xxp_init_stage1(b4);			/* full reset, re-init to "no-irq" state */
	free_irq(pdev->irq, b4);

err_out_del_from_card_array:
	for (x=0; x < MAX_B4_CARDS; x++) {
		if (cards[x] == b4) {
			b4->cardno = -1;
			cards[x] = NULL;
			break;
		}
	}

	if (x >= MAX_B4_CARDS)
		dev_err(&pdev->dev, "b4 struct @ %p should be in cards array but isn't?!\n", b4);

err_out_free_mem:
	pci_set_drvdata(pdev, NULL);
	pci_iounmap(pdev, b4->ioaddr);
	pci_iounmap(pdev, b4->addr);
	kfree(b4);

err_out_release_regions:
	pci_release_regions(pdev);

err_out_disable_pdev:
	pci_disable_device(pdev);

	return ret;
}

static void __devexit b4xxp_remove(struct pci_dev *pdev)
{
	struct b4xxp *b4 = pci_get_drvdata(pdev);
	int i;

	if (b4) {
		b4->shutdown = 1;

		for (i=b4->numspans - 1; i >= 0; i--) {
			dahdi_unregister(&b4->spans[i].span);
		}

		b4xxp_init_stage1(b4);
		free_irq(pdev->irq, b4);
		pci_set_drvdata(pdev, NULL);
		pci_iounmap(pdev, b4->ioaddr);
		pci_iounmap(pdev, b4->addr);
		pci_release_regions(pdev);
		pci_disable_device(pdev);

		b4->ioaddr = b4->addr = NULL;

		tasklet_kill(&b4->b4xxp_tlet);

		kfree(b4);
	}

	dev_info(&pdev->dev, "Driver unloaded.\n");
	return;
}

static struct pci_device_id b4xx_ids[] __devinitdata =
{
	{ 0xd161, 0xb410, PCI_ANY_ID, PCI_ANY_ID, 0, 0, (unsigned long)&wcb4xxp },
	{ 0x1397, 0x16b8, 0x1397, 0xb552, 0, 0, (unsigned long)&hfc8s },
	{ 0x1397, 0x16b8, 0x1397, 0xb55b, 0, 0, (unsigned long)&hfc8s },
	{ 0x1397, 0x08b4, 0x1397, 0xb520, 0, 0, (unsigned long)&hfc4s },
	{ 0x1397, 0x08b4, 0x1397, 0xb550, 0, 0, (unsigned long)&hfc4s },
	{ 0x1397, 0x08b4, 0x1397, 0xb752, 0, 0, (unsigned long)&hfc4s },
	{ 0x1397, 0x08b4, 0x1397, 0xb556, 0, 0, (unsigned long)&hfc2s },
	{ 0x1397, 0x08b4, 0x1397, 0xe884, 0, 0, (unsigned long)&hfc2s_OV },
	{ 0x1397, 0x08b4, 0x1397, 0xe888, 0, 0, (unsigned long)&hfc4s_OV },
	{ 0x1397, 0x16b8, 0x1397, 0xe998, 0, 0, (unsigned long)&hfc8s_OV },
	{ 0x1397, 0x08b4, 0x1397, 0xb566, 0, 0, (unsigned long)&hfc2s_BN },
	{ 0x1397, 0x08b4, 0x1397, 0xb761, 0, 0, (unsigned long)&hfc2s_BN },
	{ 0x1397, 0x08b4, 0x1397, 0xb560, 0, 0, (unsigned long)&hfc4s_BN },
	{ 0x1397, 0x08b4, 0x1397, 0xb550, 0, 0, (unsigned long)&hfc4s_BN },
	{ 0x1397, 0x16b8, 0x1397, 0xb562, 0, 0, (unsigned long)&hfc8s_BN },
	{ 0x1397, 0x16b8, 0x1397, 0xb56b, 0, 0, (unsigned long)&hfc8s_BN },
	{ 0x1397, 0x08b4, 0x1397, 0xb540, 0, 0, (unsigned long)&hfc4s_SW },
	{ 0x1397, 0x08b4, 0x1397, 0x08b4, 0, 0, (unsigned long)&hfc4s_EV },
	{0, }

};

static struct pci_driver b4xx_driver = {
	.name = "wcb4xxp",
	.probe = b4xx_probe,
	.remove = __devexit_p(b4xxp_remove),
	.id_table = b4xx_ids,
};

static int __init b4xx_init(void)
{

#ifdef CREATE_WCB4XXP_PROCFS_ENTRY
	if (!(myproc = create_proc_read_entry(PROCFS_NAME, 0444, NULL,
		                             b4xxp_proc_read, cards))) {
		printk(KERN_ERR "%s: ERROR: Could not initialize /proc/%s\n",THIS_MODULE->name, PROCFS_NAME);
	}
#endif
	if (dahdi_pci_module(&b4xx_driver))
		return -ENODEV;

	return 0;
}

static void __exit b4xx_exit(void)
{
#ifdef CREATE_WCB4XXP_PROCFS_ENTRY
	remove_proc_entry(PROCFS_NAME, NULL);
#endif
	pci_unregister_driver(&b4xx_driver);
}

module_param(debug, int, S_IRUGO | S_IWUSR);
module_param(spanfilter, int, S_IRUGO | S_IWUSR);
#ifdef LOOKBACK_SUPPORTED
module_param(loopback, int, S_IRUGO | S_IWUSR);
#endif
module_param(milliwatt, int, S_IRUGO | S_IWUSR);
module_param(pedanticpci, int, S_IRUGO);
module_param(teignorered, int, S_IRUGO | S_IWUSR);
module_param(alarmdebounce, int, S_IRUGO | S_IWUSR);
module_param(vpmsupport, int, S_IRUGO);
module_param(timer_1_ms, int, S_IRUGO | S_IWUSR);
module_param(timer_3_ms, int, S_IRUGO | S_IWUSR);
module_param(alawoverride, int, S_IRUGO | S_IWUSR);

MODULE_PARM_DESC(debug, "bitmap: 1=general 2=dtmf 4=regops 8=fops 16=ec 32=st state 64=hdlc 128=alarm");
MODULE_PARM_DESC(spanfilter, "debug filter for spans. bitmap: 1=port 1, 2=port 2, 4=port 3, 8=port 4");
#ifdef LOOKBACK_SUPPORTED
MODULE_PARM_DESC(loopback, "TODO: bitmap: 1=loop back S/T port 2=loop back DAHDI");
#endif
MODULE_PARM_DESC(milliwatt, "1=replace outgoing S/T data with mu-law milliwatt");
MODULE_PARM_DESC(pedanticpci, "1=disable PCI back-to-back transfers and flush all PCI writes immediately");
MODULE_PARM_DESC(teignorered, "1=ignore (do not inform DAHDI) if a red alarm exists in TE mode");
MODULE_PARM_DESC(alarmdebounce, "msec to wait before set/clear alarm condition");
MODULE_PARM_DESC(vpmsupport, "1=enable hardware EC, 0=disable hardware EC");
MODULE_PARM_DESC(timer_1_ms, "NT: msec to wait for link activation, TE: unused.");
MODULE_PARM_DESC(timer_3_ms, "TE: msec to wait for link activation, NT: unused.");

MODULE_AUTHOR("Digium Incorporated <support@digium.com>");
MODULE_DESCRIPTION("B410P & Similars multi-port BRI module driver.");
MODULE_LICENSE("GPL");

MODULE_DEVICE_TABLE(pci, b4xx_ids);

module_init(b4xx_init);
module_exit(b4xx_exit);
