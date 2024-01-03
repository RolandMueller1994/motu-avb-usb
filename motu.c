// SPDX-License-Identifier: GPL-2.0-only
/*
 * Motu AVB driver
 * Copyright (c) Clemens Ladisch <clemens@ladisch.de> (original UA101/UA1000 driver)
 * Copyright (c) Ralf Beck <ralfbeck1@gmx.de> (adaptation for Motu AVB series)
 * Copyright (c) Roland Mueller <roland.mueller.1994@gmx.de> (adaptation for Motu AVB ESS. Variable URB sizes, start_frame checks, ...)
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/audio.h>
#include <linux/usb/audio-v2.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include "usbaudio.h"
#include "midi.h"

MODULE_DESCRIPTION("Motu AVB driver");
MODULE_AUTHOR("Roland Mueller <roland.mueller.1994@gmx.de>");
MODULE_LICENSE("GPL v2");

/*
 * Should not be lower than the minimum scheduling delay of the host
 * controller.  Some Intel controllers need more than one frame; as long as
 * that driver doesn't tell us about this, use 1.5 frames just to be sure.
 */
#define MIN_QUEUE_LENGTH	2
/* Somewhat random. */
#define MAX_QUEUE_LENGTH	30
/*
 * This magic value optimizes memory usage efficiency for the Motu's packet
 * sizes at all sample rates, taking into account the stupid cache pool sizes
 * that usb_alloc_coherent() uses.
 */
#define DEFAULT_QUEUE_LENGTH	24

#define MAX_PACKET_SIZE		1728 /* hardware specific */
#define MAX_MEMORY_BUFFERS	DIV_ROUND_UP(MAX_QUEUE_LENGTH, \
					     PAGE_SIZE / MAX_PACKET_SIZE)

#define VENDOR_STRING_ID	1
#define PRODUCT_STRING_ID	2

#define INTCLOCK 1
#define URB_PACKS 8

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

/* module parameters */
static unsigned int queue_length = DEFAULT_QUEUE_LENGTH;
static bool midi = 0;
static unsigned int samplerate = 44100;
static bool vendor = 0;
static unsigned int ins = 0;
static unsigned int outs = 0;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "card index");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "enable card");
module_param(queue_length, uint, 0644);
MODULE_PARM_DESC(queue_length, "USB queue length in microframes, "
		 __stringify(MIN_QUEUE_LENGTH)"-"__stringify(MAX_QUEUE_LENGTH));
module_param(midi, bool, 0444);
MODULE_PARM_DESC(midi, "device has midi ports");
module_param(samplerate, uint, 0444);
MODULE_PARM_DESC(samplerate, "samplerate");
module_param(vendor, bool, 0444);
MODULE_PARM_DESC(vendor, "vendor");

module_param(ins, uint, 0444);
MODULE_PARM_DESC(ins, "ins");
module_param(outs, uint, 0444);
MODULE_PARM_DESC(outs, "outs");

enum {
        INTF_AUDIOCONTROL,
	INTF_PLAYBACK,
	INTF_CAPTURE,
        INTF_VENDOR,
	INTF_MIDI,
	INTF_VENDOR_OUT,
	INTF_VENDOR_IN,
	INTF_UNUSED,

	INTF_COUNT
};

/* bits in struct motu_avb::states */
enum {
	USB_CAPTURE_RUNNING,
	USB_PLAYBACK_RUNNING,
	ALSA_CAPTURE_OPEN,
	ALSA_PLAYBACK_OPEN,
	ALSA_CAPTURE_RUNNING,
	ALSA_PLAYBACK_RUNNING,
	CAPTURE_URB_COMPLETED,
	PLAYBACK_URB_COMPLETED,
	DISCONNECTED,
};

//			struct usb_iso_packet_descriptor iso_frame_desc[1];

struct motu_avb;

struct motu_avb_urb {
	struct urb * urb;
	struct motu_avb * motu;
	unsigned int packets;
	unsigned int buffer_size;
	unsigned int packet_size[8];
	//struct usb_iso_packet_descriptor iso_frame_desc[1];
	struct list_head ready_list;
};

struct motu_avb {
	struct usb_device *dev;
	struct snd_card *card;
	struct usb_interface *intf[INTF_COUNT];
	int card_index;
	struct snd_pcm *pcm;
	struct list_head midi_list;
	u64 format_bit;
	unsigned int rate;
    bool samplerate_is_set;
	unsigned int packets_per_second;
	spinlock_t lock;
	struct mutex mutex;
	unsigned long states;

	/* FIFO to synchronize playback rate to capture rate */
	unsigned int rate_feedback_start;
	unsigned int rate_feedback_count;
	//u8 rate_feedback[DEFAULT_QUEUE_LENGTH * 8];

	struct list_head ready_playback_urbs;
	wait_queue_head_t alsa_capture_wait;
	wait_queue_head_t rate_feedback_wait;
	wait_queue_head_t alsa_playback_wait;
	struct motu_avb_stream {
		struct snd_pcm_substream *substream;
		unsigned int usb_pipe;
		unsigned int channels;
		unsigned int frame_bytes;
		unsigned int max_packet_bytes;
		unsigned int period_pos;
		unsigned int buffer_pos;
		unsigned int queue_length;
		struct motu_avb_urb urbs[DEFAULT_QUEUE_LENGTH];
	} capture, playback;
	
	struct packet_info {
		unsigned int packet_size[URB_PACKS];
		unsigned int packets;
	} next_packet[DEFAULT_QUEUE_LENGTH];
	
	unsigned int next_playback_frame;
};

static DEFINE_MUTEX(devices_mutex);
static unsigned int devices_used;
static struct usb_driver motu_avb_driver;

static void abort_alsa_playback(struct motu_avb *motu);
static void abort_alsa_capture(struct motu_avb *motu);

static inline void add_with_wraparound(struct motu_avb *motu,
				       unsigned int *value, unsigned int add)
{
	*value += add;
	if (*value == motu->playback.queue_length)
		*value = 0;
	else if (*value > motu->playback.queue_length)
		*value -= motu->playback.queue_length;
}

static void set_samplerate(struct motu_avb *motu)
{
        __le32 data;
/*        unsigned char data1; */
        int err;
/*        int count = 100; */

        void *data_buf = NULL;

	if (motu->samplerate_is_set)
		return;

        data = cpu_to_le32(motu->rate);

        data_buf = kmemdup(&data, sizeof(data), GFP_KERNEL);
	if (!data_buf)
		return;      

	err = usb_control_msg(motu->dev, usb_sndctrlpipe(motu->dev, 0), UAC2_CS_CUR,
			      USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT,
			      UAC2_CS_CONTROL_SAM_FREQ << 8,
			      0 | (INTCLOCK << 8),
			      data_buf, sizeof(data),
			      USB_CTRL_SET_TIMEOUT);
	if (err < 0)
		dev_warn(&motu->dev->dev,
				 "%s(): cannot set samplerate %d\n",
				   __func__,  motu->rate);

        motu->samplerate_is_set = true;

        dev_warn(&motu->dev->dev, "Motu driver 0.4\n");

        kfree(data_buf);

        msleep(500);
/*
	do {
		msleep(100);

		err = usb_control_msg(motu->dev, usb_rcvctrlpipe(motu->dev, 0), UAC2_CS_CUR,
				      	USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_IN,
					UAC2_CS_CONTROL_CLOCK_VALID << 8,
					0 | (INTCLOCK << 8),
				        &data1, sizeof(data1),
					USB_CTRL_GET_TIMEOUT);
		if (err < 0) {
			dev_warn(&motu->dev->dev,
				 "%s(): cannot get clock validity for id %d, error = %d\n",
				   __func__,  data1, err);
		}
	}
	while (count--);
*/
}

static const char *usb_error_string(int err)
{
	switch (err) {
	case -ENODEV:
		return "no device";
	case -ENOENT:
		return "endpoint not enabled";
	case -EPIPE:
		return "endpoint stalled";
	case -ENOSPC:
		return "not enough bandwidth";
	case -ESHUTDOWN:
		return "device disabled";
	case -EHOSTUNREACH:
		return "device suspended";
	case -EINVAL:
		return "einval";
	case -EAGAIN:
		return "again";
	case -EFBIG:
		return "big";
	case -EMSGSIZE:
		return "msgsize";
	default:
		return "unknown error";
	}
}

static void abort_usb_capture(struct motu_avb *motu)
{
	if (test_and_clear_bit(USB_CAPTURE_RUNNING, &motu->states)) {
		wake_up(&motu->alsa_capture_wait);
		wake_up(&motu->rate_feedback_wait);
	}
}

static void abort_usb_playback(struct motu_avb *motu)
{
	if (test_and_clear_bit(USB_PLAYBACK_RUNNING, &motu->states))
		wake_up(&motu->alsa_playback_wait);
}

/* copy data from the ALSA ring buffer into the URB buffer */
static int copy_playback_data(struct motu_avb_stream *stream, struct urb *urb,
			       unsigned int frames, bool running)
{
	struct snd_pcm_runtime *runtime;
	struct snd_pcm_substream *subs;
	struct motu_avb_urb * ctx;
	struct packet_info * cur_packet;
	
	unsigned int frame_bytes, frames1, i, cur_frames, counts = 0, avail = 0, buffer_pos, period_pos;
	const u8 *source;
	unsigned int hwptr;
	bool period_elapsed = false, buffer_overflow = false;
	
	ctx = (struct motu_avb_urb *)urb->context;

	subs = stream->substream;
	runtime = stream->substream->runtime;
	frame_bytes = stream->frame_bytes;
	hwptr = stream->buffer_pos / frame_bytes;

	if (running) {
		/* calculate the byte offset-in-buffer of the appl_ptr */
		avail = (runtime->control->appl_ptr - runtime->hw_ptr_base)
			% runtime->buffer_size;
		if (avail <= hwptr)
			avail += runtime->buffer_size;
		avail -= hwptr;
	}
	
	cur_packet = &ctx->motu->next_packet[ctx->motu->rate_feedback_start];
	add_with_wraparound(ctx->motu, &ctx->motu->rate_feedback_start, 1);
	
	ctx->packets = cur_packet->packets;
	
	buffer_pos = stream->buffer_pos;
	period_pos = stream->period_pos;
	

	for (i = 0; i < cur_packet->packets; i++) {
		cur_frames = cur_packet->packet_size[i];
		ctx->packet_size[i] = cur_frames;
		urb->iso_frame_desc[i].length = cur_frames * frame_bytes;
		urb->iso_frame_desc[i].offset = counts * frame_bytes;
		if (running) {
			source = runtime->dma_area + buffer_pos * frame_bytes;
			if (buffer_pos + cur_frames <= runtime->buffer_size) {
				memcpy(urb->transfer_buffer + counts * frame_bytes, source, cur_frames * frame_bytes);
			} else {
				/* wrap around at end of ring buffer */
				frames1 = runtime->buffer_size - buffer_pos;
				memcpy(urb->transfer_buffer + counts * frame_bytes, source, frames1 * frame_bytes);
				memcpy(urb->transfer_buffer + counts * frame_bytes + frames1 * frame_bytes,
					   runtime->dma_area, (cur_frames - frames1) * frame_bytes);
			}

			buffer_pos += cur_frames;
			if (buffer_pos >= runtime->buffer_size) {
				buffer_pos -= runtime->buffer_size;
				buffer_overflow = true;
			}
			period_pos += cur_frames;
			if (period_pos >= runtime->period_size) {
				period_pos -= runtime->period_size;
				if (period_elapsed)
					printk(KERN_WARNING "More than one period used\n");
				period_elapsed = true;
			}
		} else {
			memset(urb->transfer_buffer + counts * frame_bytes, 0,
			       urb->iso_frame_desc[i].length);
		}
		counts += cur_frames;
	}
	if (counts > avail) {
		printk(KERN_WARNING "Not enough bytes avail %u needed %u\n", avail, counts * frame_bytes);
		return -1;
	}
	stream->buffer_pos = buffer_pos;
	stream->period_pos = period_pos;
	if (period_elapsed)
		return 1;
	return 0;
}

static void playback_tasklet(struct motu_avb *data)
{
	//struct motu_avb *motu = (void *)data;
	struct motu_avb *motu = data;
	struct motu_avb_urb *ctx;
	unsigned long flags;
	unsigned int frames = 0;
	struct motu_avb_urb *urb;
	bool do_period_elapsed = false;
	int copy_status;
	int err, i;
	
	

	if (unlikely(!test_bit(USB_PLAYBACK_RUNNING, &motu->states)))
		return;

	/*
	 * Synchronizing the playback rate to the capture rate is done by using
	 * the same sequence of packet sizes for both streams.
	 * Submitting a playback URB therefore requires both a ready URB and
	 * the size of the corresponding capture packet, i.e., both playback
	 * and capture URBs must have been completed.  Since the USB core does
	 * not guarantee that playback and capture complete callbacks are
	 * called alternately, we use two FIFOs for packet sizes and read URBs;
	 * submitting playback URBs is possible as long as both FIFOs are
	 * nonempty.
	 */
	spin_lock_irqsave(&motu->lock, flags);
	while (motu->rate_feedback_count > 0 &&
	       !list_empty(&motu->ready_playback_urbs)) {

		/* take URB out of FIFO */
		urb = list_first_entry(&motu->ready_playback_urbs,
				       struct motu_avb_urb, ready_list);
		list_del(&urb->ready_list);

		/* fill packet with data or silence */
		copy_status = copy_playback_data(&motu->playback,
							urb->urb,
							frames,
							test_bit(ALSA_PLAYBACK_RUNNING, &motu->states));
		if (copy_status > 0)
			do_period_elapsed = true;
		else if (copy_status < 0)
			break;
		for (i = 0; i < urb->packets; i++)
			frames += urb->packet_size[i];
		motu->rate_feedback_count--;
		/* and off you go ... */
		err = usb_submit_urb(urb->urb, GFP_ATOMIC);
		if (unlikely(err < 0)) {
			spin_unlock_irqrestore(&motu->lock, flags);
			abort_usb_playback(motu);
			abort_alsa_playback(motu);
			dev_err(&motu->dev->dev, "USB request error %d: %s\n",
				err, usb_error_string(err));
			return;
		}
		motu->playback.substream->runtime->delay += frames;
	}
	spin_unlock_irqrestore(&motu->lock, flags);
	if (do_period_elapsed) {
		snd_pcm_period_elapsed(motu->playback.substream);
	}
}

static void playback_urb_complete(struct urb *urb)
{
	//struct motu_avb_urb *urb = (struct motu_avb_urb *)usb_urb;
	struct motu_avb *motu = ((struct motu_avb_urb *)urb->context)->motu;
	unsigned long flags;
	unsigned int frames = 0, i;

	if (unlikely(urb->status == -ENOENT ||	/* unlinked */
		     urb->status == -ENODEV ||	/* device removed */
		     urb->status == -ECONNRESET ||	/* unlinked */
		     urb->status == -ESHUTDOWN)) {	/* device disabled */
		abort_usb_playback(motu);
		abort_alsa_playback(motu);
		return;
	}
	
	if ((urb->start_frame & 0x3ff) != motu->next_playback_frame) {
		printk(KERN_WARNING "ISOC delay %u!\n", (urb->start_frame & 0x3ff) - motu->next_playback_frame);
	}
	motu->next_playback_frame = (urb->start_frame + urb->number_of_packets) & 0x3ff;

	if (test_bit(USB_PLAYBACK_RUNNING, &motu->states)) {
		/* append URB to FIFO */
		spin_lock_irqsave(&motu->lock, flags);
		list_add_tail(&((struct motu_avb_urb *)urb->context)->ready_list, &motu->ready_playback_urbs);
		for (i = 0; i < urb->number_of_packets; i++)
			frames += urb->iso_frame_desc[i].length;
		motu->playback.substream->runtime->delay -= frames / motu->playback.frame_bytes;
		spin_unlock_irqrestore(&motu->lock, flags);
		if (motu->rate_feedback_count > 0)
			playback_tasklet(motu);
	}
}

static void first_playback_urb_complete(struct urb *urb)
{
	struct motu_avb *motu = ((struct motu_avb_urb *)urb->context)->motu;
	
	motu->next_playback_frame = urb->start_frame & 0x3ff;

	urb->complete = playback_urb_complete;
	playback_urb_complete(urb);

	set_bit(PLAYBACK_URB_COMPLETED, &motu->states);
	wake_up(&motu->alsa_playback_wait);
}

/* copy data from the URB buffer into the ALSA ring buffer */
static bool copy_capture_data(struct motu_avb_stream *stream, struct urb *urb,
			      unsigned int frames)
{
	struct snd_pcm_runtime *runtime;
	unsigned int frame_bytes, frames1, i, cur_frames;
	u8 *dest;
	bool do_period_elapsed = false;

	runtime = stream->substream->runtime;
	frame_bytes = stream->frame_bytes;
	for (i = 0; i < urb->number_of_packets; i++) {
		dest = runtime->dma_area + stream->buffer_pos * frame_bytes;
		cur_frames = urb->iso_frame_desc[i].actual_length / frame_bytes;
		if (stream->buffer_pos + cur_frames <= runtime->buffer_size) {
			memcpy(dest, urb->transfer_buffer + urb->iso_frame_desc[i].offset, cur_frames * frame_bytes);
		} else {
			/* wrap around at end of ring buffer */
			frames1 = runtime->buffer_size - stream->buffer_pos;
			memcpy(dest, urb->transfer_buffer + urb->iso_frame_desc[i].offset, frames1 * frame_bytes);
			memcpy(runtime->dma_area,
				   urb->transfer_buffer + urb->iso_frame_desc[i].offset + frames1 * frame_bytes,
				   (cur_frames - frames1) * frame_bytes);
		}
		stream->buffer_pos += cur_frames;
		if (stream->buffer_pos >= runtime->buffer_size)
			stream->buffer_pos -= runtime->buffer_size;
		stream->period_pos += cur_frames;
		if (stream->period_pos >= runtime->period_size) {
			stream->period_pos -= runtime->period_size;
			do_period_elapsed = true;
		}
	}
	return do_period_elapsed;
}

static void capture_urb_complete(struct urb *urb)
{
	struct motu_avb *motu = ((struct motu_avb_urb *)urb->context)->motu;
	struct motu_avb_stream *stream = &motu->capture;
	struct packet_info * out_packet;
	//struct motu_avb_stream *playback_stream = &motu->playback;
	unsigned long flags;
	unsigned int frames, write_ptr;
	bool do_period_elapsed = false;
	int err, i;

	if (unlikely(urb->status == -ENOENT ||		/* unlinked */
		     urb->status == -ENODEV ||		/* device removed */
		     urb->status == -ECONNRESET ||	/* unlinked */
		     urb->status == -ESHUTDOWN))	/* device disabled */
		goto stream_stopped;

	frames = 0;
	for(i = 0; i < urb->number_of_packets; i++) {
		if (urb->status >= 0 && urb->iso_frame_desc[i].status >= 0) {
			frames += urb->iso_frame_desc[i].actual_length /
				stream->frame_bytes;
		}
	}

	spin_lock_irqsave(&motu->lock, flags);

	if (frames > 0 && test_bit(ALSA_CAPTURE_RUNNING, &motu->states))
		do_period_elapsed = copy_capture_data(stream, urb, frames);
	else
		do_period_elapsed = false;

	if (test_bit(USB_CAPTURE_RUNNING, &motu->states)) {
		/* append packet size to FIFO */
		write_ptr = motu->rate_feedback_start;
		add_with_wraparound(motu, &write_ptr, motu->rate_feedback_count);
		out_packet = &motu->next_packet[write_ptr];
		out_packet->packets = urb->number_of_packets;
		for (i = 0; i< urb->number_of_packets; i++) {
			if (urb->iso_frame_desc[i].status == 0) 
				out_packet->packet_size[i] = urb->iso_frame_desc[i].actual_length / stream->frame_bytes;
			else
				out_packet->packet_size[i] = 0;	
		}

		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (unlikely(err < 0)) {
			spin_unlock_irqrestore(&motu->lock, flags);
			dev_err(&motu->dev->dev, "USB request error %d: %s\n",
				err, usb_error_string(err));
			goto stream_stopped;
		}

		if (motu->rate_feedback_count < motu->playback.queue_length) {
			motu->rate_feedback_count++;
			if (motu->rate_feedback_count ==
						motu->playback.queue_length)
				wake_up(&motu->rate_feedback_wait);
		} else {
			/*
			 * Ring buffer overflow; this happens when the playback
			 * stream is not running.  Throw away the oldest entry,
			 * so that the playback stream, when it starts, sees
			 * the most recent packet sizes.
			 */
			add_with_wraparound(motu, &motu->rate_feedback_start, 1);
		}
	}

	spin_unlock_irqrestore(&motu->lock, flags);

	if (do_period_elapsed)
		snd_pcm_period_elapsed(stream->substream);
		
	if (test_bit(USB_PLAYBACK_RUNNING, &motu->states) &&
		    !list_empty(&motu->ready_playback_urbs))
		    playback_tasklet(motu);
	return;

stream_stopped:
	abort_usb_playback(motu);
	abort_usb_capture(motu);
	abort_alsa_playback(motu);
	abort_alsa_capture(motu);
}

static void first_capture_urb_complete(struct urb *urb)
{
	struct motu_avb *motu = ((struct motu_avb_urb *)urb->context)->motu;

	urb->complete = capture_urb_complete;
	capture_urb_complete(urb);

	set_bit(CAPTURE_URB_COMPLETED, &motu->states);
	wake_up(&motu->alsa_capture_wait);
	wake_up(&motu->alsa_playback_wait);
}

static int submit_stream_urbs(struct motu_avb *motu, struct motu_avb_stream *stream)
{
	unsigned int i;
	struct urb *urb;

	for (i = 0; i < stream->queue_length; ++i) {
		urb = stream->urbs[i].urb;
		int err = usb_submit_urb(stream->urbs[i].urb, GFP_KERNEL);
		if (err < 0) {
			dev_err(&motu->dev->dev, "USB request error %d: %s\n",
				err, usb_error_string(err));
			return err;
		}
	}
	return 0;
}

static void kill_stream_urbs(struct motu_avb_stream *stream)
{
	unsigned int i;

	for (i = 0; i < stream->queue_length; ++i)
		usb_kill_urb(stream->urbs[i].urb);
}

static int enable_iso_interface(struct motu_avb *motu, unsigned int intf_index)
{
	struct usb_host_interface *alts;

	alts = motu->intf[intf_index]->cur_altsetting;
	if (alts->desc.bAlternateSetting != 1) {
		int err = usb_set_interface(motu->dev,
					    alts->desc.bInterfaceNumber, 1);
		if (err < 0) {
			dev_err(&motu->dev->dev,
				"cannot initialize interface; error %d: %s\n",
				err, usb_error_string(err));
			return err;
		}
	}
	return 0;
}

static void disable_iso_interface(struct motu_avb *motu, unsigned int intf_index)
{
	struct usb_host_interface *alts;

	if (!motu->intf[intf_index])
		return;

	alts = motu->intf[intf_index]->cur_altsetting;
	if (alts->desc.bAlternateSetting != 0) {
		int err = usb_set_interface(motu->dev,
					    alts->desc.bInterfaceNumber, 0);
		if (err < 0 && !test_bit(DISCONNECTED, &motu->states))
			dev_warn(&motu->dev->dev,
				 "interface reset failed; error %d: %s\n",
				 err, usb_error_string(err));
	}
}

static void stop_usb_capture(struct motu_avb *motu)
{
	clear_bit(USB_CAPTURE_RUNNING, &motu->states);
	printk(KERN_WARNING "Stop capture called\n");

	kill_stream_urbs(&motu->capture);

	if (vendor)
	{
		disable_iso_interface(motu, INTF_VENDOR_IN);
	}
	else
	{
		disable_iso_interface(motu, INTF_CAPTURE);
	}
}

static int start_usb_capture(struct motu_avb *motu)
{
	int err = 0;
	
	printk(KERN_WARNING "Start capture called\n");

	if (test_bit(DISCONNECTED, &motu->states))
		return -ENODEV;

	if (test_bit(USB_CAPTURE_RUNNING, &motu->states))
		return 0;

	kill_stream_urbs(&motu->capture);

	if (vendor)
	{
		enable_iso_interface(motu, INTF_VENDOR_IN);
	}
	else
	{
		enable_iso_interface(motu, INTF_CAPTURE);
	}
	if (err < 0)
		return err;

	clear_bit(CAPTURE_URB_COMPLETED, &motu->states);
	motu->capture.urbs[0].urb->complete = first_capture_urb_complete;
	motu->rate_feedback_start = 0;
	motu->rate_feedback_count = 0;

	set_bit(USB_CAPTURE_RUNNING, &motu->states);
	printk(KERN_WARNING "Before submit stream\n");
	err = submit_stream_urbs(motu, &motu->capture);
	if (err < 0)
		stop_usb_capture(motu);
	return err;
}

static void stop_usb_playback(struct motu_avb *motu)
{
	printk(KERN_WARNING "Stop playback called\n");
	clear_bit(USB_PLAYBACK_RUNNING, &motu->states);

	kill_stream_urbs(&motu->playback);

	if (vendor)
	{
		disable_iso_interface(motu, INTF_VENDOR_OUT);
	}
	else
	{
		disable_iso_interface(motu, INTF_PLAYBACK);
	}
}

static int start_usb_playback(struct motu_avb *motu)
{
	printk(KERN_WARNING "Start playback called\n");
	unsigned int i, frames, frame_offset, isoc_no; // i, 
	struct urb *urb;
	int err = 0;

	if (test_bit(DISCONNECTED, &motu->states))
		return -ENODEV;

	clear_bit(USB_PLAYBACK_RUNNING, &motu->states);

	kill_stream_urbs(&motu->playback);

	if (vendor)
	{
		disable_iso_interface(motu, INTF_VENDOR_OUT);
	}
	else
	{
		disable_iso_interface(motu, INTF_PLAYBACK);
	}
	if (err < 0)
		return err;

	clear_bit(PLAYBACK_URB_COMPLETED, &motu->states);
	motu->playback.urbs[0].urb->complete =
		first_playback_urb_complete;
	spin_lock_irq(&motu->lock);
	INIT_LIST_HEAD(&motu->ready_playback_urbs);

        /* reset rate feedback */
	motu->rate_feedback_start = 0;
	motu->rate_feedback_count = 0;
	spin_unlock_irq(&motu->lock);

	if (vendor)
	{
		enable_iso_interface(motu, INTF_VENDOR_OUT);
	}
	else
	{
		enable_iso_interface(motu, INTF_PLAYBACK);
	}
	if (err < 0)
		return err;

	/*
	 * We submit the initial URBs all at once, so we have to wait for the
	 * packet size FIFO to be full.
	 */
	wait_event(motu->rate_feedback_wait,
		   motu->rate_feedback_count >= 0 ||
		   !test_bit(USB_CAPTURE_RUNNING, &motu->states) ||
		   test_bit(DISCONNECTED, &motu->states));
	if (test_bit(DISCONNECTED, &motu->states)) {
		stop_usb_playback(motu);
		return -ENODEV;
	}
	if (!test_bit(USB_CAPTURE_RUNNING, &motu->states)) {
		stop_usb_playback(motu);
		return -EIO;
	}

    spin_lock_irq(&motu->lock);

	for (i = 0; i < motu->playback.queue_length; i++) {
		list_add_tail(&motu->playback.urbs[i].ready_list, &motu->ready_playback_urbs);
	}

	spin_unlock_irq(&motu->lock);
	set_bit(USB_PLAYBACK_RUNNING, &motu->states);
	wake_up(&motu->alsa_playback_wait);

	return 0;
}

static void abort_alsa_capture(struct motu_avb *motu)
{
	if (test_bit(ALSA_CAPTURE_RUNNING, &motu->states))
		snd_pcm_stop_xrun(motu->capture.substream);
}

static void abort_alsa_playback(struct motu_avb *motu)
{
	if (test_bit(ALSA_PLAYBACK_RUNNING, &motu->states))
		snd_pcm_stop_xrun(motu->playback.substream);
}

static int set_stream_hw(struct motu_avb *motu, struct snd_pcm_substream *substream,
			 unsigned int channels)
{
	int err;

	substream->runtime->hw.info =
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_FIFO_IN_FRAMES;
	substream->runtime->hw.formats = SNDRV_PCM_FMTBIT_S24_3LE;
	substream->runtime->hw.rates = snd_pcm_rate_to_rate_bit(motu->rate);
	substream->runtime->hw.rate_min = motu->rate;
	substream->runtime->hw.rate_max = motu->rate;
	substream->runtime->hw.channels_min = channels;
	substream->runtime->hw.channels_max = channels;
	substream->runtime->hw.buffer_bytes_max = 45000 * 1024;
	substream->runtime->hw.period_bytes_min = 1;
	substream->runtime->hw.period_bytes_max = UINT_MAX;
	substream->runtime->hw.periods_min = 2;
	substream->runtime->hw.periods_max = UINT_MAX;
	err = snd_pcm_hw_constraint_minmax(substream->runtime,
					   SNDRV_PCM_HW_PARAM_PERIOD_TIME,
					   1500000 / motu->packets_per_second,
					   UINT_MAX);
	if (err < 0)
		return err;
	err = snd_pcm_hw_constraint_msbits(substream->runtime, 0, 32, 24);
	return err;
}

static int capture_pcm_open(struct snd_pcm_substream *substream)
{
	struct motu_avb *motu = substream->private_data;
	int err;

        dev_err(&motu->dev->dev, "capture_open\n");

        set_samplerate(motu);

	motu->capture.substream = substream;
	err = set_stream_hw(motu, substream, motu->capture.channels);
	if (err < 0)
		return err;
	substream->runtime->hw.fifo_size =
		DIV_ROUND_CLOSEST(motu->rate, motu->packets_per_second);
	substream->runtime->delay = substream->runtime->hw.fifo_size;

	mutex_lock(&motu->mutex);
	err = start_usb_capture(motu);
	if (err >= 0)
		set_bit(ALSA_CAPTURE_OPEN, &motu->states);
	mutex_unlock(&motu->mutex);
	return err;
}

static int playback_pcm_open(struct snd_pcm_substream *substream)
{
	struct motu_avb *motu = substream->private_data;
	int err;

        dev_err(&motu->dev->dev, "playback_open\n");

        set_samplerate(motu);

	motu->playback.substream = substream;
	err = set_stream_hw(motu, substream, motu->playback.channels);
	if (err < 0)
		return err;
	substream->runtime->hw.fifo_size =
		DIV_ROUND_CLOSEST(motu->rate * motu->playback.queue_length,
				  motu->packets_per_second);

	mutex_lock(&motu->mutex);
	err = start_usb_capture(motu);
	if (err < 0)
		goto error;
	err = start_usb_playback(motu);
	if (err < 0) {
		if (!test_bit(ALSA_CAPTURE_OPEN, &motu->states))
			stop_usb_capture(motu);
		goto error;
	}
	set_bit(ALSA_PLAYBACK_OPEN, &motu->states);
error:
	mutex_unlock(&motu->mutex);
	return err;
}

static int capture_pcm_close(struct snd_pcm_substream *substream)
{
	struct motu_avb *motu = substream->private_data;

	mutex_lock(&motu->mutex);
	clear_bit(ALSA_CAPTURE_OPEN, &motu->states);
	if (!test_bit(ALSA_PLAYBACK_OPEN, &motu->states))
		stop_usb_capture(motu);
	mutex_unlock(&motu->mutex);
	return 0;
}

static int playback_pcm_close(struct snd_pcm_substream *substream)
{
	struct motu_avb *motu = substream->private_data;

	mutex_lock(&motu->mutex);
	stop_usb_playback(motu);
	clear_bit(ALSA_PLAYBACK_OPEN, &motu->states);
	if (!test_bit(ALSA_CAPTURE_OPEN, &motu->states))
		stop_usb_capture(motu);
	mutex_unlock(&motu->mutex);
	return 0;
}

static int alloc_stream_urbs(struct motu_avb *motu, struct motu_avb_stream *stream,
			     void (*urb_complete)(struct urb *))
{
	unsigned max_packet_size = stream->max_packet_bytes;
	struct motu_avb_urb *urb_ctx;
	unsigned int urb_no, urb_packs, isoc_no; // b, u = 0, 
	//unsigned int period_size = params_period_size(params);
	
	stream->queue_length = queue_length;
	//printk(KERN_WARNING "Period size %u\n", period_size);
	urb_packs = URB_PACKS;
	
	for (urb_no = 0; urb_no < stream->queue_length; urb_no++) {
		urb_ctx = &stream->urbs[urb_no];
		urb_ctx->motu = motu;
		urb_ctx->packets = urb_packs;
		urb_ctx->buffer_size = max_packet_size * urb_packs;
		
		urb_ctx->urb = usb_alloc_urb(urb_ctx->packets, GFP_KERNEL);
		if (!urb_ctx->urb)
			return -ENOMEM;
		usb_init_urb(urb_ctx->urb);	
		
		urb_ctx->urb->transfer_buffer = usb_alloc_coherent(motu->dev, urb_ctx->buffer_size, GFP_KERNEL, &urb_ctx->urb->transfer_dma);
		if (!urb_ctx->urb->transfer_buffer)
			return -ENOMEM;
		urb_ctx->urb->dev = motu->dev;
		urb_ctx->urb->pipe = stream->usb_pipe;
		urb_ctx->urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
		urb_ctx->urb->interval = 1;
		urb_ctx->urb->transfer_buffer_length = urb_packs * max_packet_size;
		urb_ctx->urb->number_of_packets = urb_packs;
		urb_ctx->urb->context = urb_ctx;
		urb_ctx->urb->complete = urb_complete;
		for (isoc_no = 0; isoc_no < urb_packs; isoc_no++) {
			urb_ctx->urb->iso_frame_desc[isoc_no].offset = isoc_no*max_packet_size;
			urb_ctx->urb->iso_frame_desc[isoc_no].length = max_packet_size;
		}
	}

	return 0;
}

static int capture_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *hw_params)
{
	struct motu_avb *motu = substream->private_data;
	int err = 0;
	
	printk(KERN_WARNING "capture_hw_params\n");

	mutex_lock(&motu->mutex);
	err = start_usb_capture(motu);
	mutex_unlock(&motu->mutex);
	return err;
}

static int playback_pcm_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *hw_params)
{
	struct motu_avb *motu = substream->private_data;
	int err = 0;
	
	printk(KERN_WARNING "playback_pcm_hw_params\n");

	mutex_lock(&motu->mutex);
	if (err >= 0)
		err = start_usb_playback(motu);
	mutex_unlock(&motu->mutex);
	return err;
}

static int capture_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct motu_avb *motu = substream->private_data;
	int err;
	
	printk(KERN_WARNING "capture_pcm_prepare\n");

	mutex_lock(&motu->mutex);
	err = start_usb_capture(motu);
	mutex_unlock(&motu->mutex);
	if (err < 0)
		return err;

	/*
	 * The EHCI driver schedules the first packet of an iso stream at 10 ms
	 * in the future, i.e., no data is actually captured for that long.
	 * Take the wait here so that the stream is known to be actually
	 * running when the start trigger has been called.
	 */
	wait_event(motu->alsa_capture_wait,
		   test_bit(CAPTURE_URB_COMPLETED, &motu->states) ||
		   !test_bit(USB_CAPTURE_RUNNING, &motu->states));
	if (test_bit(DISCONNECTED, &motu->states))
		return -ENODEV;
	if (!test_bit(USB_CAPTURE_RUNNING, &motu->states))
		return -EIO;

	motu->capture.period_pos = 0;
	motu->capture.buffer_pos = 0;
	return 0;
}

static int playback_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct motu_avb *motu = substream->private_data;
	int err; 
	
	printk(KERN_WARNING "playback_pcm_prepare\n");

	mutex_lock(&motu->mutex);
	err = start_usb_capture(motu);
	if (err >= 0)
		err = start_usb_playback(motu);		
	mutex_unlock(&motu->mutex);
	if (err < 0)
		return err;

	/* see the comment in capture_pcm_prepare() */
	/*wait_event(motu->alsa_playback_wait,
		   test_bit(CAPTURE_URB_COMPLETED, &motu->states) ||
		   !test_bit(USB_PLAYBACK_RUNNING, &motu->states));*/
	wait_event(motu->alsa_playback_wait,
	   test_bit(CAPTURE_URB_COMPLETED, &motu->states) ||
	   !test_bit(USB_PLAYBACK_RUNNING, &motu->states));
	if (test_bit(DISCONNECTED, &motu->states))
		return -ENODEV;
	if (!test_bit(USB_PLAYBACK_RUNNING, &motu->states))
		return -EIO;

	substream->runtime->delay = 0;
	motu->playback.period_pos = 0;
	motu->playback.buffer_pos = 0;
	return 0;
}

static int capture_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct motu_avb *motu = substream->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (!test_bit(USB_CAPTURE_RUNNING, &motu->states))
			return -EIO;
		set_bit(ALSA_CAPTURE_RUNNING, &motu->states);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		clear_bit(ALSA_CAPTURE_RUNNING, &motu->states);
		return 0;
	default:
		return -EINVAL;
	}
}

static int playback_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct motu_avb *motu = substream->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (!test_bit(USB_PLAYBACK_RUNNING, &motu->states))
			return -EIO;
		set_bit(ALSA_PLAYBACK_RUNNING, &motu->states);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		clear_bit(ALSA_PLAYBACK_RUNNING, &motu->states);
		return 0;
	default:
		return -EINVAL;
	}
}

static inline snd_pcm_uframes_t motu_avb_pcm_pointer(struct motu_avb *motu,
						  struct motu_avb_stream *stream)
{
	unsigned long flags;
	unsigned int pos;

	spin_lock_irqsave(&motu->lock, flags);
	pos = stream->buffer_pos;
	spin_unlock_irqrestore(&motu->lock, flags);
	return pos;
}

static snd_pcm_uframes_t capture_pcm_pointer(struct snd_pcm_substream *subs)
{
	struct motu_avb *motu = subs->private_data;

	return motu_avb_pcm_pointer(motu, &motu->capture);
}

static snd_pcm_uframes_t playback_pcm_pointer(struct snd_pcm_substream *subs)
{
	struct motu_avb *motu = subs->private_data;

	return motu_avb_pcm_pointer(motu, &motu->playback);
}

static const struct snd_pcm_ops capture_pcm_ops = {
	.open = capture_pcm_open,
	.close = capture_pcm_close,
	.hw_params = capture_pcm_hw_params,
	.prepare = capture_pcm_prepare,
	.trigger = capture_pcm_trigger,
	.pointer = capture_pcm_pointer,
};

static const struct snd_pcm_ops playback_pcm_ops = {
	.open = playback_pcm_open,
	.close = playback_pcm_close,
	.hw_params = playback_pcm_hw_params,
	.prepare = playback_pcm_prepare,
	.trigger = playback_pcm_trigger,
	.pointer = playback_pcm_pointer,
};

static int detect_usb_format(struct motu_avb *motu)
{
        const struct usb_endpoint_descriptor *epd;

        motu->format_bit = SNDRV_PCM_FMTBIT_S24_3LE;
        motu->rate = samplerate;
	motu->packets_per_second = 8000;
	
	printk(KERN_WARNING "detect usb\n");

	if (vendor && (samplerate <= 48000))
	{
		motu->capture.channels = 64;
		motu->playback.channels = 64;
        }
        else if (vendor && (samplerate <= 96000))
        {
		motu->capture.channels = 32;
		motu->playback.channels = 32;
        }
        else
        {
		motu->capture.channels = 24;
		motu->playback.channels = 24;
        }

        if (ins > 0)
        	motu->capture.channels = ins;

        if (outs > 0)
        	motu->playback.channels = outs;

	motu->capture.frame_bytes = motu->capture.channels*3;
	motu->playback.frame_bytes = motu->playback.channels*3;

	if (vendor)
	{
		epd = &motu->intf[INTF_VENDOR_IN]->altsetting[1].endpoint[0].desc;
	}
	else
	{
		epd = &motu->intf[INTF_CAPTURE]->altsetting[1].endpoint[0].desc;
	}

	if (!usb_endpoint_is_isoc_in(epd)) {
		dev_err(&motu->dev->dev, "invalid capture endpoint\n");
		return -ENXIO;
	}
	motu->capture.usb_pipe = usb_rcvisocpipe(motu->dev, usb_endpoint_num(epd));
	motu->capture.max_packet_bytes = usb_endpoint_maxp_mult(epd)*usb_endpoint_maxp(epd);

        dev_err(&motu->dev->dev, "max packets capture endpoint %d\n", motu->capture.max_packet_bytes);

	if (vendor)
	{
		epd = &motu->intf[INTF_VENDOR_OUT]->altsetting[1].endpoint[0].desc;
	}
	else
	{
		epd = &motu->intf[INTF_PLAYBACK]->altsetting[1].endpoint[0].desc;
	}

	if (!usb_endpoint_is_isoc_out(epd)) {
		dev_err(&motu->dev->dev, "invalid playback endpoint\n");
		return -ENXIO;
	}
	motu->playback.usb_pipe = usb_sndisocpipe(motu->dev, usb_endpoint_num(epd));
	motu->playback.max_packet_bytes = usb_endpoint_maxp_mult(epd)*usb_endpoint_maxp(epd);

        dev_err(&motu->dev->dev, "max packets playback endpoint %d\n", motu->playback.max_packet_bytes);

	return 0;
}

static void free_stream_urbs(struct motu_avb_stream *stream)
{
	unsigned int urb_no; // i, 
	struct motu_avb_urb * urb_ctx;

	for (urb_no = 0; urb_no < stream->queue_length; urb_no++) {
		urb_ctx = &stream->urbs[urb_no];
		if (urb_ctx->urb && urb_ctx->buffer_size) 
			usb_free_coherent(urb_ctx->motu->dev, urb_ctx->buffer_size, urb_ctx->urb->transfer_buffer, urb_ctx->urb->transfer_dma);
		usb_free_urb(urb_ctx->urb);
		urb_ctx->urb = NULL;
		urb_ctx->buffer_size = 0;
	}
}

static void free_usb_related_resources(struct motu_avb *motu,
				       struct usb_interface *interface)
{
	unsigned int i;
	struct usb_interface *intf;

	mutex_lock(&motu->mutex);
	free_stream_urbs(&motu->capture);
	free_stream_urbs(&motu->playback);
	mutex_unlock(&motu->mutex);

	for (i = 0; i < ARRAY_SIZE(motu->intf); ++i) {
		mutex_lock(&motu->mutex);
		intf = motu->intf[i];
		motu->intf[i] = NULL;
		mutex_unlock(&motu->mutex);
		if (intf) {
			usb_set_intfdata(intf, NULL);
			if (intf != interface)
				usb_driver_release_interface(&motu_avb_driver,
							     intf);
		}
	}
}

static void motu_avb_card_free(struct snd_card *card)
{
	struct motu_avb *motu = card->private_data;

	mutex_destroy(&motu->mutex);
}

static int motu_avb_probe(struct usb_interface *interface,
		       const struct usb_device_id *usb_id)
{
	static const struct snd_usb_midi_endpoint_info midi_ep = {
		.out_cables = 0x0001,
		.in_cables = 0x0001
	};
	static const struct snd_usb_audio_quirk midi_quirk = {
		.type = QUIRK_MIDI_FIXED_ENDPOINT,
		.data = &midi_ep
	};
	static const int intf_numbers[2][8] = {
		{	/* AVB devices without MIDI */
                        [INTF_AUDIOCONTROL] = 0,
			[INTF_PLAYBACK] = 1,
			[INTF_CAPTURE] = 2,
                        [INTF_VENDOR] = 3,
			[INTF_VENDOR_OUT] = 4,
			[INTF_VENDOR_IN] = 5,
			[INTF_UNUSED] = -1,
			[INTF_MIDI] = -1,
		},
		{	/* AVB devices with MIDI */
                        [INTF_AUDIOCONTROL] = 0,
			[INTF_PLAYBACK] = 1,
			[INTF_CAPTURE] = 2,
			[INTF_UNUSED] = 3,
			[INTF_MIDI] = 4,
                        [INTF_VENDOR] = 5,
			[INTF_VENDOR_OUT] = 6,
			[INTF_VENDOR_IN] = 7,
		},
	};
	struct snd_card *card;
	struct motu_avb *motu;
	unsigned int card_index, i;
	const char *name;
	char usb_path[32];
	int err;

	if (interface->altsetting->desc.bInterfaceNumber !=
	    intf_numbers[midi][0])
		return -ENODEV;

	mutex_lock(&devices_mutex);

	for (card_index = 0; card_index < SNDRV_CARDS; ++card_index)
		if (enable[card_index] && !(devices_used & (1 << card_index)))
			break;
	if (card_index >= SNDRV_CARDS) {
		mutex_unlock(&devices_mutex);
		return -ENOENT;
	}
	err = snd_card_new(&interface->dev,
			   index[card_index], id[card_index], THIS_MODULE,
			   sizeof(*motu), &card);
	if (err < 0) {
		mutex_unlock(&devices_mutex);
		return err;
	}
	card->private_free = motu_avb_card_free;
	motu = card->private_data;
	motu->dev = interface_to_usbdev(interface);
	motu->card = card;
	motu->card_index = card_index;
        motu->samplerate_is_set = false;
	INIT_LIST_HEAD(&motu->midi_list);
	spin_lock_init(&motu->lock);
	mutex_init(&motu->mutex);
	INIT_LIST_HEAD(&motu->ready_playback_urbs);
	init_waitqueue_head(&motu->alsa_capture_wait);
	init_waitqueue_head(&motu->rate_feedback_wait);
	init_waitqueue_head(&motu->alsa_playback_wait);

    dev_info(&motu->dev->dev, "samplerate = %d, queue_length = %d, midi = %d, vendor = %d\n", samplerate, queue_length, midi, vendor);

	motu->intf[0] = interface;
	for (i = 1; i < ARRAY_SIZE(motu->intf); ++i) {
		dev_info(&motu->dev->dev, "probing interface %d\n", intf_numbers[midi][i]);
		if (intf_numbers[midi][i] > 0)
		{

			motu->intf[i] = usb_ifnum_to_if(motu->dev,
						      intf_numbers[midi][i]);
			if (!motu->intf[i]) {
				dev_err(&motu->dev->dev, "interface %u not found\n",
					intf_numbers[midi][i]);
				err = -ENXIO;
				goto probe_error;
			}
			err = usb_driver_claim_interface(&motu_avb_driver,
							 motu->intf[i], motu);
			if (err < 0) {
				motu->intf[i] = NULL;
				err = -EBUSY;
				goto probe_error;
			}
		}

	}
	dev_info(&motu->dev->dev, "probing interfaces sucessful\n");

	err = detect_usb_format(motu);
	if (err < 0)
		goto probe_error;

	name = "MOTU-AVB";
	strcpy(card->driver, "MOTU-AVB");
	strcpy(card->shortname, name);
	usb_make_path(motu->dev, usb_path, sizeof(usb_path));
	snprintf(motu->card->longname, sizeof(motu->card->longname),
		 "MOTU %s", motu->dev->product);

	err = alloc_stream_urbs(motu, &motu->capture, capture_urb_complete);
	if (err < 0)
		goto probe_error;
	err = alloc_stream_urbs(motu, &motu->playback, playback_urb_complete);
	if (err < 0)
		goto probe_error;

	err = snd_pcm_new(card, name, 0, 1, 1, &motu->pcm);
	if (err < 0)
		goto probe_error;
	motu->pcm->private_data = motu;
	strcpy(motu->pcm->name, name);
	snd_pcm_set_ops(motu->pcm, SNDRV_PCM_STREAM_PLAYBACK, &playback_pcm_ops);
	snd_pcm_set_ops(motu->pcm, SNDRV_PCM_STREAM_CAPTURE, &capture_pcm_ops);
	snd_pcm_set_managed_buffer_all(motu->pcm, SNDRV_DMA_TYPE_VMALLOC,
				       NULL, 0, 0);

	if (motu->intf[INTF_MIDI] > 0)
	{
		err = snd_usbmidi_create(card, motu->intf[INTF_MIDI],
					 &motu->midi_list, &midi_quirk);
		if (err < 0)
			goto probe_error;
	}

	err = snd_card_register(card);
	if (err < 0)
		goto probe_error;

	usb_set_intfdata(interface, motu);
	devices_used |= 1 << card_index;

	mutex_unlock(&devices_mutex);
	return 0;

probe_error:
	free_usb_related_resources(motu, interface);
	snd_card_free(card);
	mutex_unlock(&devices_mutex);
	return err;
}

static void motu_avb_disconnect(struct usb_interface *interface)
{
	struct motu_avb *motu = usb_get_intfdata(interface);
	struct list_head *midi;

	if (!motu)
		return;

	mutex_lock(&devices_mutex);

	set_bit(DISCONNECTED, &motu->states);
	wake_up(&motu->rate_feedback_wait);

	/* make sure that userspace cannot create new requests */
	snd_card_disconnect(motu->card);

	/* make sure that there are no pending USB requests */
	list_for_each(midi, &motu->midi_list)
		snd_usbmidi_disconnect(midi);
	abort_alsa_playback(motu);
	abort_alsa_capture(motu);
	mutex_lock(&motu->mutex);
	stop_usb_playback(motu);
	stop_usb_capture(motu);
	mutex_unlock(&motu->mutex);

	free_usb_related_resources(motu, interface);

	devices_used &= ~(1 << motu->card_index);

	snd_card_free_when_closed(motu->card);

	mutex_unlock(&devices_mutex);
}

static const struct usb_device_id motu_avb_ids[] = {
	{ USB_DEVICE(0x07fd, 0x0005) }, /* Motu AVB */
	{ }
};
MODULE_DEVICE_TABLE(usb, motu_avb_ids);

static struct usb_driver motu_avb_driver = {
	.name = "motu",
	.id_table = motu_avb_ids,
	.probe = motu_avb_probe,
	.disconnect = motu_avb_disconnect,
#if 0
	.suspend = motu_avb_suspend,
	.resume = motu_avb_resume,
#endif
};

module_usb_driver(motu_avb_driver);
