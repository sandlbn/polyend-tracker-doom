/* OpenTracker addition to the Teensy 3.x core (see PATCHES.txt).
 * Same permissive terms as the rest of this directory (LICENSE.txt).
 *
 * Bulk endpoint driver of the USB mass-storage interface (usb_msc.h). Not a
 * header for general use: usb_dev.c includes it once, inside its
 * `#ifdef MSC_INTERFACE`, after its BDT definitions (bdt_t, table, index(),
 * BDT_DESC(), stat2bufferdescriptor(), TX/RX/ODD/EVEN), so the OT_USB_MSC=0
 * build compiles exactly the upstream file. The host test
 * reimpl/tests/usb_msc_driver_test.cpp includes it the same way against a
 * simulated USB controller.
 */
// OPENTRACKER PATCH (see PATCHES.txt): bulk endpoints of the mass-storage
// interface (usb_msc.h). Packets go by DMA straight into / out of the queued
// segments; both BDT banks of an endpoint stay armed while data is queued.
// Data toggles are tracked here (DATA0 after configuration and after
// CLEAR_FEATURE(ENDPOINT_HALT), USB 2.0 9.4.5), not derived from the bank.
// All of it runs in usb_isr() context.
typedef struct {
	uint8_t *buf;
	uint32_t len;      // segment length
	uint32_t armed;    // bytes handed to BDTs
	uint32_t done;     // bytes completed
	uint8_t complete;
} msc_seg_t;
// Segments per direction: the protocol keeps up to 4 data chunks in flight
// (src/core/usb/msc_bot.h kMaxChunks) so the bus keeps moving through a long
// main-loop pass.
#define MSC_SEGS 4
typedef struct {
	msc_seg_t seg[MSC_SEGS];
	uint8_t head;      // oldest slot
	uint8_t count;     // slots in use (queued, or complete and not yet polled)
	uint8_t bank;      // next BDT bank to arm
	uint8_t donebank;  // bank the controller completes next
	uint8_t inflight;  // BDTs armed (0..2)
	uint8_t toggle;    // DATA0/DATA1 of the next armed packet
} msc_dir_t;
static msc_dir_t msc_dir[2];   // [RX], [TX]
// Receive tails: a BDT armed with BC < 64 overflows (DMAERR) when the host
// sends a full packet, so a receive of less than 64 bytes is armed with
// BC = 64 into a bank's scratch and copied into the segment on completion.
// DMA memory (SRAM_U, uncached): usb_dev.c's packet buffers are not used here.
#ifndef USB_MSC_DMA_MEM
#define USB_MSC_DMA_MEM __attribute__((section(".dma"), aligned(32)))
#endif
static USB_MSC_DMA_MEM uint8_t msc_rx_tail[2][64];
static uint8_t *msc_rx_tail_dst[2];   // per bank: segment bytes the scratch stands for
static uint8_t msc_rx_tail_len[2];
static void (*msc_hook)(uint8_t event) = NULL;
#define MSC_EP(tx) ((tx) ? MSC_TX_ENDPOINT : MSC_RX_ENDPOINT)
#define MSC_ENDPT(tx) (*(volatile uint8_t *)(&USB0_ENDPT0 + MSC_EP(tx) * 4))

void usb_msc_set_hook(void (*hook)(uint8_t event)) { msc_hook = hook; }

static void msc_arm(int tx)
{
	msc_dir_t *d = &msc_dir[tx];
	while (d->inflight < 2) {
		msc_seg_t *s = NULL;
		uint32_t i, n;
		for (i = 0; i < d->count; i++) {
			msc_seg_t *c = &d->seg[(d->head + i) & (MSC_SEGS - 1)];
			if (!c->complete && c->armed < c->len) { s = c; break; }
		}
		if (!s) return;
		n = s->len - s->armed;
		if (n > 64) n = 64;
		if (tx == RX && n < 64) {
			msc_rx_tail_dst[d->bank] = s->buf + s->armed;
			msc_rx_tail_len[d->bank] = (uint8_t)n;
			table[index(MSC_EP(tx), tx, d->bank)].addr = msc_rx_tail[d->bank];
			table[index(MSC_EP(tx), tx, d->bank)].desc = BDT_DESC(64, d->toggle);
		} else {
			table[index(MSC_EP(tx), tx, d->bank)].addr = s->buf + s->armed;
			table[index(MSC_EP(tx), tx, d->bank)].desc = BDT_DESC(n, d->toggle);
		}
		s->armed += n;
		d->bank ^= 1;
		d->toggle ^= 1;
		d->inflight++;
	}
}

// Drop the armed BDTs of one direction; the incomplete segments re-arm from
// what actually completed. Only from contexts where the SIE does not use these
// BDTs (SETUP handling freezes token processing; bus reset).
static void msc_disarm(int tx)
{
	msc_dir_t *d = &msc_dir[tx];
	uint32_t i;
	table[index(MSC_EP(tx), tx, EVEN)].desc = 0;
	table[index(MSC_EP(tx), tx, ODD)].desc = 0;
	d->bank = d->donebank;
	if (d->inflight & 1) d->toggle ^= 1;   // toggles of the dropped packets were not used
	d->inflight = 0;
	for (i = 0; i < MSC_SEGS; i++) d->seg[i].armed = d->seg[i].done;
}

static void msc_drop(int tx)
{
	msc_dir_t *d = &msc_dir[tx];
	msc_disarm(tx);
	d->head = 0;
	d->count = 0;
	memset(d->seg, 0, sizeof(d->seg));
}

static void msc_event(uint8_t ev) { if (msc_hook) msc_hook(ev); }

int usb_msc_rx_queue(uint8_t *buf, uint32_t len)
{
	msc_dir_t *d = &msc_dir[RX];
	msc_seg_t *s;
	if (d->count >= MSC_SEGS || len == 0) return 0;
	s = &d->seg[(d->head + d->count) & (MSC_SEGS - 1)];
	s->buf = buf; s->len = len; s->armed = 0; s->done = 0; s->complete = 0;
	d->count++;
	msc_arm(RX);
	return 1;
}

int usb_msc_rx_poll(uint32_t *got)
{
	msc_dir_t *d = &msc_dir[RX];
	msc_seg_t *s = &d->seg[d->head];
	if (!d->count || !s->complete) return 0;
	if (got) *got = s->done;
	s->complete = 0;
	d->head = (d->head + 1) & (MSC_SEGS - 1);
	d->count--;
	return 1;
}

int usb_msc_tx_queue(const uint8_t *buf, uint32_t len)
{
	msc_dir_t *d = &msc_dir[TX];
	msc_seg_t *s;
	if (d->count >= MSC_SEGS || len == 0) return 0;
	s = &d->seg[(d->head + d->count) & (MSC_SEGS - 1)];
	s->buf = (uint8_t *)buf; s->len = len; s->armed = 0; s->done = 0; s->complete = 0;
	d->count++;
	msc_arm(TX);
	return 1;
}

int usb_msc_tx_poll(void)
{
	msc_dir_t *d = &msc_dir[TX];
	msc_seg_t *s = &d->seg[d->head];
	if (!d->count || !s->complete) return 0;
	s->complete = 0;
	d->head = (d->head + 1) & (MSC_SEGS - 1);
	d->count--;
	return 1;
}

void usb_msc_stall(int in) { MSC_ENDPT(in ? TX : RX) |= USB_ENDPT_EPSTALL; }
int usb_msc_stalled(int in) { return (MSC_ENDPT(in ? TX : RX) & USB_ENDPT_EPSTALL) != 0; }
int usb_msc_configured(void) { return usb_configuration != 0; }

void usb_msc_token(uint8_t stat)
{
	int tx = (stat & 0x08) ? TX : RX;
	const uint8_t bank = (stat & 0x04) ? ODD : EVEN;
	msc_dir_t *d = &msc_dir[tx];
	bdt_t *b = stat2bufferdescriptor(stat);
	uint32_t n = (b->desc >> 16) & 0x3FF, i;
	uint8_t *at = (uint8_t *)b->addr;
	int tail = 0;
	if (d->inflight) d->inflight--;
	// The controller uses the other bank next (its ODD bit flips on every
	// completed BDT): taken from the token itself, so a disagreement could
	// never outlive one packet (review F3).
	d->donebank = bank ^ 1;
	if (tx == RX && at == msc_rx_tail[bank]) {
		at = msc_rx_tail_dst[bank];
		tail = 1;
	}
	// Attributed by address: the packet belongs to the oldest incomplete
	// segment whose next byte it was armed for. A BDT left from a dropped
	// segment (usb_msc_rx_flush(), a reset) matches none and is ignored.
	for (i = 0; i < d->count; i++) {
		msc_seg_t *s = &d->seg[(d->head + i) & (MSC_SEGS - 1)];
		if (s->complete || at != s->buf + s->done) continue;
		if (tail) {
			// Only the bytes the segment asked for (a host that sends more
			// broke the protocol; the rest is dropped).
			if (n > msc_rx_tail_len[bank]) n = msc_rx_tail_len[bank];
			memcpy(at, msc_rx_tail[bank], n);
		}
		s->done += n;
		// A short packet ends a receive segment early (the CBW, 31 bytes).
		// A BDT still armed into the rest of it is left to
		// usb_msc_rx_flush() (msc_bot.cpp: before the CSW, while the host
		// sends nothing).
		if (s->done >= s->len || (tx == RX && n < 64)) {
			s->complete = 1;
			s->armed = s->done;
		}
		break;
	}
	msc_arm(tx);
	msc_event(USB_MSC_EVENT_POLL);
}

void usb_msc_rx_flush(void) { msc_drop(RX); }
void usb_msc_tx_flush(void) { msc_drop(TX); }

void usb_msc_sof(void) { msc_event(USB_MSC_EVENT_POLL); }

static volatile uint8_t msc_kicked = 0;
void usb_msc_kick(void)
{
	msc_kicked = 1;
	NVIC_SET_PENDING(IRQ_USBOTG);
}

void usb_msc_bus_reset(void)
{
	memset(msc_dir, 0, sizeof(msc_dir));   // USBRST: ODDRST puts every bank back to even
	table[index(MSC_RX_ENDPOINT, RX, EVEN)].desc = 0;
	table[index(MSC_RX_ENDPOINT, RX, ODD)].desc = 0;
	table[index(MSC_TX_ENDPOINT, TX, EVEN)].desc = 0;
	table[index(MSC_TX_ENDPOINT, TX, ODD)].desc = 0;
	msc_event(USB_MSC_EVENT_RESET);
}

void usb_msc_configure(void)
{
	msc_drop(RX);
	msc_drop(TX);
	msc_dir[RX].toggle = 0;
	msc_dir[TX].toggle = 0;
	msc_event(USB_MSC_EVENT_RESET);
}

void usb_msc_clear_halt(int in)
{
	int tx = in ? TX : RX;
	msc_disarm(tx);
	msc_dir[tx].toggle = 0;
	msc_arm(tx);
	msc_event(USB_MSC_EVENT_POLL);
}

void usb_msc_class_reset(void)
{
	// Bulk-Only Mass Storage Reset (BOT 3.1): ready for the next CBW. Halt
	// features and data toggles stay as they are.
	msc_drop(RX);
	msc_drop(TX);
	msc_event(USB_MSC_EVENT_RESET);
}
