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

// #define DEBUG

MODULE_DESCRIPTION("Motu AVB ESS Driver");
MODULE_AUTHOR("Roland Mueller <roland.mueller.1994@gmx.de>");
MODULE_LICENSE("GPL v2");

#define DEFAULT_QUEUE_LENGTH	24

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
static bool vendor = 0;
static unsigned int channels = 0;
static int time_silent = 2;
static bool frame_check = 0;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "card index");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "enable card");
module_param(midi, bool, 0444);
MODULE_PARM_DESC(midi, "Device has midi ports");
module_param(vendor, bool, 0444);
MODULE_PARM_DESC(vendor, "Enable vendor mode");
module_param(channels, uint, 0444);
MODULE_PARM_DESC(channels, "Number of channels in vendor mode. This can be used if the number of channels is changed on the interface.");
module_param(time_silent, int, 0444);
MODULE_PARM_DESC(time_silent, "Defines for how many milli-seconds silent is sent at the beginning of the playback stream. If not set, 2ms are used.");
module_param(frame_check, bool, 0444);
MODULE_PARM_DESC(frame_check, "Enables start_frame check in playback stream. This will reset the interface in case a gap in the isoc stream is detected.");

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

struct motu_avb;

struct motu_avb_urb {
	struct urb * urb;
	struct motu_avb * motu;
	unsigned int packets;
	unsigned int buffer_size;
	unsigned int packet_size[8];
	struct list_head ready_list;
	bool was_silent;
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

	struct list_head ready_playback_urbs;
	wait_queue_head_t alsa_capture_wait;
	wait_queue_head_t rate_feedback_wait;
	wait_queue_head_t alsa_playback_wait;
	struct motu_avb_stream {
		struct snd_pcm_substream *substream;
		bool first;
		unsigned int silent_urbs;
		unsigned int frame_print;
		bool start_frame_init;
		unsigned int urb_packs;
		unsigned int default_packet_size;
		unsigned int discard;
		unsigned int usb_pipe;
		unsigned int channels;
		unsigned int frame_bytes;
		unsigned int max_packet_bytes;
		unsigned int period_pos;
		unsigned int buffer_pos;
		unsigned int queue_length;
		unsigned int opened_count;
		bool needs_prepare;
		struct motu_avb_urb urbs[DEFAULT_QUEUE_LENGTH];
	} capture, playback;
	
	/* Packet size info. Offset and length information from capture urbs stored
	for later usage in playback. */
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
	int err;
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
	dev_warn(&motu->dev->dev,
			"Motu AVB ESS Driver. Module params: Midi %i, vendor %i, time_silent %i, frame_check %i\n",
			midi, vendor, time_silent, frame_check);
    kfree(data_buf);
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

static unsigned int calc_avail(struct motu_avb_stream *stream, bool running, int needed)
{
	/* Logic to calculate the available bytes in the pcm buffer. Copied from the alsa class driver */
	struct snd_pcm_runtime *runtime;
	struct snd_pcm_substream *subs;
	unsigned int frame_bytes, hwptr, avail=0;
	
	subs = stream->substream;
	runtime = stream->substream->runtime;
	frame_bytes = stream->frame_bytes;
	hwptr = stream->buffer_pos;

	struct motu_avb *motu = subs->private_data;

	if (running) {
		/* calculate the byte offset-in-buffer of the appl_ptr */
		avail = (runtime->control->appl_ptr - runtime->hw_ptr_base)
			% runtime->buffer_size;
		if (avail <= hwptr)
			avail += runtime->buffer_size;
		avail -= hwptr;
	}
#ifdef DEBUG
	if (avail < needed) {
		dev_err(&motu->dev->dev,
				"Avail %u , appl %u, hw_ptr_base %u, hwptr %u, buffersize %u, needed %i\n",
				avail, runtime->control->appl_ptr, runtime->hw_ptr_base, hwptr,
				runtime->buffer_size, needed);
	}
#endif

	return avail;
}

static bool check_avail(struct motu_avb_stream *stream, unsigned int needed, bool running) 
{
	unsigned int avail = calc_avail(stream, running, 0);
	
	return needed < avail;
}

/* copy data from the ALSA ring buffer into the URB buffer */
static int copy_playback_data(struct motu_avb_stream *stream, struct urb *urb,
			       unsigned int frames, bool running)
{
	struct snd_pcm_runtime *runtime;
	struct snd_pcm_substream *subs;
	struct motu_avb_urb * ctx;
	struct packet_info * cur_packet;
	
	unsigned int frame_bytes, frames1, i, cur_frames, counts = 0, avail = 0, buffer_pos, period_pos, periods=0;
	const u8 *source;
	unsigned int hwptr;
	bool period_elapsed = false, buffer_overflow = false;
	
	ctx = (struct motu_avb_urb *)urb->context;

	subs = stream->substream;
	runtime = stream->substream->runtime;
	frame_bytes = stream->frame_bytes;
	hwptr = stream->buffer_pos / frame_bytes;
	
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
				period_elapsed = true;
				periods += 1;
			}
		} else {
			memset(urb->transfer_buffer + counts * frame_bytes, 0,
			       urb->iso_frame_desc[i].length);
		}
		counts += cur_frames;
	}
	avail = calc_avail(stream, running, counts);
	if (counts > avail) {
		/* 
		In case there was not enough data in the pcm ring buffer, we just send silence.
		This is done such that there are no dropouts in playback stream which might cause
		channel hopping or decimated sound.
		*/	
#ifdef DEBUG
		dev_err(&ctx->motu->dev->dev, "Sending zeros: Counts %u Avail %u\n", counts, avail);
#endif
		counts = 0;
		for (i = 0; i < cur_packet->packets; i++) {
			cur_frames = cur_packet->packet_size[i];
			ctx->packet_size[i] = cur_frames;
			urb->iso_frame_desc[i].length = cur_frames * frame_bytes;
			urb->iso_frame_desc[i].offset = counts * frame_bytes;
			
			memset(urb->transfer_buffer + counts * frame_bytes, 0,
				urb->iso_frame_desc[i].length);
			
			counts += cur_frames;
		}
		return -1;
	}
	stream->buffer_pos = buffer_pos;
	stream->period_pos = period_pos;
#ifdef DEBUG
	if (periods > 1) {
		dev_err(&ctx->motu->dev->dev, "More than one period: %u\n", periods);
	}
#endif

	if (period_elapsed)
		return 1;
	return 0;
}

static void playback_tasklet(struct motu_avb *data)
{
	struct motu_avb *motu = data;
	unsigned long flags;
	unsigned int frames = 0, counts, frame_bytes;
	struct motu_avb_urb *urb;
	bool do_period_elapsed = false;
	int copy_status;
	int err, i;
	bool silent;
	
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

		silent = false;
		if (motu->playback.silent_urbs > 0) {
			/*
			Sending silent urbs at the beginning of playback stream such that 2ms of data are queued. 
			Done to comply with host controller implementation.
			*/
#ifdef DEBUG
			dev_err(&motu->dev->dev, "Try sending silence, Avail: %i, Running %i\n", check_avail(&motu->playback, motu->playback.default_packet_size * motu->playback.urb_packs,
					test_bit(ALSA_PLAYBACK_RUNNING, &motu->states)), test_bit(ALSA_PLAYBACK_RUNNING, &motu->states));
#endif
			if (check_avail(&motu->playback, motu->playback.default_packet_size * motu->playback.urb_packs,
					test_bit(ALSA_PLAYBACK_RUNNING, &motu->states))) {
#ifdef DEBUG
				dev_err(&motu->dev->dev, "Sending silence\n");
#endif
				/* take URB out of FIFO */
				urb = list_first_entry(&motu->ready_playback_urbs,
							   struct motu_avb_urb, ready_list);
				list_del(&urb->ready_list);

				counts = 0;
				frame_bytes = motu->playback.frame_bytes;
				for (i = 0; i < motu->playback.urb_packs; i++) {
					urb->urb->iso_frame_desc[i].length = motu->playback.default_packet_size * frame_bytes;
					urb->urb->iso_frame_desc[i].offset = counts * frame_bytes;
					memset(urb->urb->transfer_buffer + urb->urb->iso_frame_desc[i].offset, 0,
					   urb->urb->iso_frame_desc[i].length);
					counts += motu->playback.default_packet_size;
				}
				motu->playback.silent_urbs--;
				silent = true;
			} else {
				spin_unlock_irqrestore(&motu->lock, flags);
				return;
			}
		} else {
			/* fill packet with data or silence */
			/* take URB out of FIFO */
			urb = list_first_entry(&motu->ready_playback_urbs,
						   struct motu_avb_urb, ready_list);
			list_del(&urb->ready_list);

			copy_status = copy_playback_data(&motu->playback,
								urb->urb,
								frames,
								test_bit(ALSA_PLAYBACK_RUNNING, &motu->states));
			if (copy_status > 0)
				do_period_elapsed = true;
			for (i = 0; i < urb->packets; i++)
				frames += urb->packet_size[i];
			motu->rate_feedback_count--;
			if (copy_status >= 0)
				silent = false;
			else
				silent = true;
		}
		/* and off you go ... */
		urb->was_silent = silent;
		err = usb_submit_urb(urb->urb, GFP_ATOMIC);
		if (unlikely(err < 0)) {
			spin_unlock_irqrestore(&motu->lock, flags);
			abort_usb_playback(motu);
			abort_alsa_playback(motu);
			dev_err(&motu->dev->dev, "USB request error %d: %s\n",
				err, usb_error_string(err));
			spin_unlock_irqrestore(&motu->lock, flags);
			return;
		}
		if (!silent)
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
	struct motu_avb_urb *urb_ctx = (struct motu_avb_urb *)urb->context;
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
	
	if (motu->playback.first) {
		motu->next_playback_frame = urb->start_frame & 0x3ff;
		motu->playback.substream->runtime->delay = 0;
	}

	if (((urb->start_frame & 0x3ff) != motu->next_playback_frame) && frame_check && !motu->playback.start_frame_init) {
		/*
		If playback urbs are submitted to late, the host controller will schedule at the frame 
		where it is able to process it. This will lead to dropouts in the iso stream which further
		leads to channel hopping and decimated sound. This shouldn't happen in the first place but
		we are able to fix it here by resetting the interface.
		
		start_frame is masked with 0x3ff as this is where the EHCI driver wraps the frame number.
		XHCI wouldn't need this.
		*/
		dev_err(&motu->dev->dev, "ISOC delay %u!\n", (urb->start_frame & 0x3ff) - motu->next_playback_frame);
		motu->playback.needs_prepare = true;
		motu->capture.needs_prepare = true;
		abort_usb_playback(motu);
		abort_alsa_playback(motu);
		abort_usb_capture(motu);
	} else if (((urb->start_frame & 0x3ff) == motu->next_playback_frame) && frame_check && motu->playback.start_frame_init) {
		dev_warn(&motu->dev->dev, "Start frame check started!\n");
		motu->playback.start_frame_init = false;
	}

#ifdef DEBUG
	if (motu->playback.frame_print > 0) {
		dev_warn(&motu->dev->dev,
				"Playback start frame: %u, Masked %u; Expected playback frame: %u\n",
				urb->start_frame, urb->start_frame & 0x3ff,
				motu->next_playback_frame);
		motu->playback.frame_print--;
	}
#endif
	// The next urb should be exactly the number of iso packets we send per urb isn the future.
	motu->next_playback_frame = (urb->start_frame + urb->number_of_packets) & 0x3ff;

	if (test_bit(USB_PLAYBACK_RUNNING, &motu->states)) {
		/* append URB to FIFO */
		spin_lock_irqsave(&motu->lock, flags);
		list_add_tail(&((struct motu_avb_urb *)urb->context)->ready_list, &motu->ready_playback_urbs);
		for (i = 0; i < urb->number_of_packets; i++)
			frames += urb->iso_frame_desc[i].length;
		if (!urb_ctx->was_silent)
			motu->playback.substream->runtime->delay -= frames / motu->playback.frame_bytes;
		spin_unlock_irqrestore(&motu->lock, flags);
		if (motu->rate_feedback_count > 0)
			playback_tasklet(motu);
	}
	
	if (motu->playback.first) {
		motu->playback.first = false;
		set_bit(PLAYBACK_URB_COMPLETED, &motu->states);
		wake_up(&motu->alsa_playback_wait);
	}
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
	
	if (stream->discard > 0) {
		stream->discard--;
		err = usb_submit_urb(urb, GFP_ATOMIC);
		spin_unlock_irqrestore(&motu->lock, flags);
		return;
	}

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
			if (motu->rate_feedback_count > 0 && !test_bit(USB_PLAYBACK_RUNNING, &motu->states)) {
				wake_up(&motu->rate_feedback_wait);
			}
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
	
	if (stream->first) {
		stream->first = false;
		set_bit(CAPTURE_URB_COMPLETED, &motu->states);
		wake_up(&motu->alsa_capture_wait);
		wake_up(&motu->alsa_playback_wait);
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
	msleep(50);
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
	msleep(50);
}

static void stop_usb_capture(struct motu_avb *motu)
{
	clear_bit(USB_CAPTURE_RUNNING, &motu->states);
#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Stop usb capture called\n");
#endif

	kill_stream_urbs(&motu->capture);

	if (vendor)
	{
		disable_iso_interface(motu, INTF_VENDOR_IN);
	}
	else
	{
		disable_iso_interface(motu, INTF_CAPTURE);
	}
	motu->samplerate_is_set = false;
}

static int start_usb_capture(struct motu_avb *motu)
{
	int err = 0;

	motu->capture.first = true;
	motu->capture.discard = 2;

	if (test_bit(DISCONNECTED, &motu->states))
		return -ENODEV;

	if (test_bit(USB_CAPTURE_RUNNING, &motu->states))
		return 0;

	stop_usb_capture(motu);
#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Start usb capture called\n");
#endif

	if (vendor)
	{
		err = enable_iso_interface(motu, INTF_VENDOR_IN);
	}
	else
	{
		err = enable_iso_interface(motu, INTF_CAPTURE);
	}

	if (err < 0) {
		dev_warn(&motu->dev->dev, "Motu AVB: Playback iso int err %i\n", err);
		return err;
	}

	clear_bit(CAPTURE_URB_COMPLETED, &motu->states);
	motu->rate_feedback_start = 0;
	motu->rate_feedback_count = 0;

	set_bit(USB_CAPTURE_RUNNING, &motu->states);
	err = submit_stream_urbs(motu, &motu->capture);
	if (err < 0)
		stop_usb_capture(motu);
	return err;
}

static void stop_usb_playback(struct motu_avb *motu)
{
	clear_bit(USB_PLAYBACK_RUNNING, &motu->states);
#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Stop usb playback called\n");
#endif

	kill_stream_urbs(&motu->playback);

	if (vendor)
	{
		disable_iso_interface(motu, INTF_VENDOR_OUT);
	}
	else
	{
		disable_iso_interface(motu, INTF_PLAYBACK);
	}
	motu->samplerate_is_set = false;
}

static int start_usb_playback(struct motu_avb *motu)
{
	unsigned int i;

	int err = 0;

	if (test_bit(DISCONNECTED, &motu->states))
		return -ENODEV;

	stop_usb_playback(motu);
#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Start usb playback called\n");
#endif

	if (vendor)
	{
		err = enable_iso_interface(motu, INTF_VENDOR_OUT);
	}
	else
	{
		err = enable_iso_interface(motu, INTF_PLAYBACK);
	}

	if (err < 0) {
		dev_warn(&motu->dev->dev, "Motu AVB: Playback iso int err %i\n", err);
		return err;
	}

	clear_bit(PLAYBACK_URB_COMPLETED, &motu->states);
	spin_lock_irq(&motu->lock);
	INIT_LIST_HEAD(&motu->ready_playback_urbs);

    /* reset rate feedback */
	motu->rate_feedback_start = 0;
	motu->rate_feedback_count = 0;
	motu->playback.period_pos = 0;
	motu->playback.buffer_pos = 0;

	for (i = 0; i < motu->playback.queue_length; i++) {
		list_add_tail(&motu->playback.urbs[i].ready_list, &motu->ready_playback_urbs);
	}

	spin_unlock_irq(&motu->lock);
	wait_event(motu->rate_feedback_wait,
		   test_bit(CAPTURE_URB_COMPLETED, &motu->states) ||
		   !test_bit(USB_CAPTURE_RUNNING, &motu->states) ||
		   test_bit(DISCONNECTED, &motu->states));
	if (test_bit(DISCONNECTED, &motu->states)) {
		stop_usb_playback(motu);
		return -ENODEV;
	}
	/*if (!test_bit(USB_CAPTURE_RUNNING, &motu->states)) {
		stop_usb_playback(motu);
		return -EIO;
	}*/

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
	unsigned int min_channels, max_channels;
	
	if (vendor && channels > 0) {
		min_channels = channels;
		max_channels = channels;
	} else if (vendor) {
		min_channels = 24;
		max_channels = 64;
	} else {
		min_channels = 24;
		max_channels = 24;
	}

	substream->runtime->hw.info =
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_FIFO_IN_FRAMES | 
		SNDRV_PCM_INFO_JOINT_DUPLEX;
	substream->runtime->hw.formats = SNDRV_PCM_FMTBIT_S24_3LE;
	substream->runtime->hw.rates = 	snd_pcm_rate_to_rate_bit(44100) | 
									snd_pcm_rate_to_rate_bit(48000) | 
									snd_pcm_rate_to_rate_bit(88200) | 
									snd_pcm_rate_to_rate_bit(96000) | 
									snd_pcm_rate_to_rate_bit(176400) | 
									snd_pcm_rate_to_rate_bit(192000);
	substream->runtime->hw.rate_min = 44100;
	substream->runtime->hw.rate_max = 192000;
	substream->runtime->hw.channels_min = min_channels;
	substream->runtime->hw.channels_max = max_channels;
	substream->runtime->hw.buffer_bytes_max = 45000 * 1024;
	substream->runtime->hw.period_bytes_min = 1;
	substream->runtime->hw.period_bytes_max = UINT_MAX;
	substream->runtime->hw.periods_min = 2;
	substream->runtime->hw.periods_max = UINT_MAX;
	err = snd_pcm_hw_constraint_minmax(substream->runtime,
					   SNDRV_PCM_HW_PARAM_PERIOD_TIME,
					   125,
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

	mutex_lock(&motu->mutex);
	if (motu->capture.opened_count > 0) {
		motu->capture.opened_count++;
		mutex_unlock(&motu->mutex);
		return 0;
	}
	mutex_unlock(&motu->mutex);
	motu->capture.opened_count++;
#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Capture pcm open\n");
#endif
        
    err = set_stream_hw(motu, substream, motu->capture.channels);
	if (err < 0)
		return err;

	motu->capture.substream = substream;

	return 0;
}

static int playback_pcm_open(struct snd_pcm_substream *substream)
{
	struct motu_avb *motu = substream->private_data;
	int err;
	
	mutex_lock(&motu->mutex);
	if (motu->playback.opened_count > 0) {
		motu->playback.opened_count++;
		mutex_unlock(&motu->mutex);
		return 0;
	}
	mutex_unlock(&motu->mutex);
	motu->playback.opened_count++;
#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Playback pcm open\n");
#endif
        
    err = set_stream_hw(motu, substream, motu->playback.channels);
	if (err < 0)
		return err;

	motu->playback.substream = substream;
	return 0;
}

static int capture_pcm_close(struct snd_pcm_substream *substream)
{
	struct motu_avb *motu = substream->private_data;

	mutex_lock(&motu->mutex);
	motu->capture.opened_count--;
	if (motu->capture.opened_count > 0)
		goto unlock;
#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Capture pcm close\n");
#endif
	motu->capture.needs_prepare = true;
	clear_bit(ALSA_CAPTURE_OPEN, &motu->states);
	if (!test_bit(ALSA_PLAYBACK_OPEN, &motu->states))
		stop_usb_capture(motu);
unlock:
	mutex_unlock(&motu->mutex);
	return 0;
}

static int playback_pcm_close(struct snd_pcm_substream *substream)
{
	struct motu_avb *motu = substream->private_data;

	mutex_lock(&motu->mutex);
	motu->playback.opened_count--;
	if (motu->playback.opened_count > 0)
		goto unlock;
#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Playback pcm close\n");
#endif
	motu->playback.needs_prepare = true;
	stop_usb_playback(motu);
	clear_bit(ALSA_PLAYBACK_OPEN, &motu->states);
	if (!test_bit(ALSA_CAPTURE_OPEN, &motu->states))
		stop_usb_capture(motu);
unlock:
	mutex_unlock(&motu->mutex);
	return 0;
}

static int alloc_stream_urbs(struct motu_avb *motu, struct motu_avb_stream *stream,
			     void (*urb_complete)(struct urb *), struct snd_pcm_hw_params *params)
{
	unsigned max_packet_size = stream->max_packet_bytes;
	struct motu_avb_urb *urb_ctx;
	unsigned int urb_no, urb_packs, isoc_no; 
	unsigned int period_size = params_period_size(params);
	unsigned int freqmax = motu->rate + (motu->rate >> 1);
	unsigned int maxsize = DIV_ROUND_UP(freqmax, 8000);

#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Period size %u\n", period_size);
#endif

	if (maxsize > max_packet_size / stream->frame_bytes) 
		maxsize = max_packet_size / stream->frame_bytes;
	
	stream->queue_length = queue_length;
	urb_packs = URB_PACKS;
	
	while (urb_packs > 1 && urb_packs * maxsize >= period_size)
		urb_packs >>= 1;
	
	stream->urb_packs = urb_packs;
	
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

static void set_channels(struct motu_avb_stream *stream, struct motu_avb *motu, unsigned int rate) {
	if (vendor && (motu->rate <= 48000))
	{
		stream->channels = 64;
    }
    else if (vendor && (motu->rate <= 96000))
    {
		stream->channels = 32;
    }
    else
    {
		stream->channels = 24;
    }
    
    if (vendor && channels > 0 && channels <= stream->channels) {
    	stream->channels = channels;
    }

	stream->frame_bytes = stream->channels*3;
}

static int capture_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *hw_params)
{
	struct motu_avb *motu = substream->private_data;
	int err = 0, err_sample = 0;
	unsigned int rate = params_rate(hw_params);
	unsigned int pcm_channels = params_channels(hw_params);
	bool rate_change = motu->rate != rate;
	
	motu->rate = rate;
	if (rate_change)
		set_samplerate(motu);
	set_channels(&motu->capture, motu, rate);
	substream->runtime->hw.fifo_size =
		DIV_ROUND_CLOSEST(motu->rate, motu->packets_per_second);
	substream->runtime->delay = substream->runtime->hw.fifo_size;
	
	if (motu->capture.channels != pcm_channels) {
		dev_warn(&motu->dev->dev, "Motu AVB: Number of channels %u not possible\n", pcm_channels);
		err = -ENXIO;
	}
	
	mutex_lock(&motu->mutex);
	err = alloc_stream_urbs(motu, &motu->capture, capture_urb_complete, hw_params);
	if (err < 0)
		goto probe_error;
	
	if (err_sample < 0) {
		mutex_unlock(&motu->mutex);
		return err_sample;
	}
	
probe_error:
	mutex_unlock(&motu->mutex);
	return err;
}

static int playback_pcm_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *hw_params)
{
	struct motu_avb *motu = substream->private_data;
	int err = 0, err_sample = 0;
	unsigned int rate = params_rate(hw_params);
	unsigned int pcm_channels = params_channels(hw_params);
	bool rate_change = motu->rate != rate;
	
	motu->rate = rate;
	if (rate_change)
		set_samplerate(motu);
	set_channels(&motu->playback, motu, rate);
	substream->runtime->hw.fifo_size =
		DIV_ROUND_CLOSEST(motu->rate * motu->playback.queue_length,
				  motu->packets_per_second);
	motu->playback.default_packet_size = DIV_ROUND_UP(rate, 8000);
	
	if (motu->playback.channels != pcm_channels) {
		dev_warn(&motu->dev->dev, "Motu AVB: Number of channels %u not possible\n", pcm_channels);
		err_sample = -ENXIO;
	}
	
	mutex_lock(&motu->mutex);
	err = alloc_stream_urbs(motu, &motu->playback, playback_urb_complete, hw_params);
	if (err < 0)
		goto probe_error;
		
	if (err_sample < 0) {
		mutex_unlock(&motu->mutex);
		return err_sample;
	}
	
probe_error:
	mutex_unlock(&motu->mutex);
	return err;
}

static int capture_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct motu_avb *motu = substream->private_data;
	int err;
	
	mutex_lock(&motu->mutex);
	if (test_bit(ALSA_CAPTURE_OPEN, &motu->states) && !motu->capture.needs_prepare) {
		mutex_unlock(&motu->mutex);
		return 0;
	}
#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Capture pcm prepare\n");
#endif
	motu->capture.needs_prepare = false;
	
	motu->capture.period_pos = 0;
	motu->capture.buffer_pos = 0;

	err = 0;
	if (!test_bit(USB_CAPTURE_RUNNING, &motu->states))
		err = start_usb_capture(motu);
	if (err >= 0)
		set_bit(ALSA_CAPTURE_OPEN, &motu->states);
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
	return 0;
}

static int playback_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct motu_avb *motu = substream->private_data;
	int err; 
	
	mutex_lock(&motu->mutex);
	if (test_bit(ALSA_PLAYBACK_OPEN, &motu->states) && !motu->playback.needs_prepare) {
		mutex_unlock(&motu->mutex);
		return 0;
	}
#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Playback pcm prepare\n");
#endif
	motu->playback.needs_prepare = false;
	
	motu->playback.first = true;
	motu->playback.discard = 0;

	unsigned int silent_urbs = time_silent * 8 / motu->playback.urb_packs;
	silent_urbs = silent_urbs > 16 ? 16 : silent_urbs;
	motu->playback.silent_urbs = silent_urbs;
	motu->playback.start_frame_init = true;

	motu->playback.frame_print = 50;
	dev_warn(&motu->dev->dev, "Motu AVB: Timeframe to send silent urbs as playback startup %i\n", time_silent);
	dev_warn(&motu->dev->dev, "Motu AVB: Number of silent urbs %u\n", motu->playback.silent_urbs);
	
	/*if (!test_bit(USB_CAPTURE_RUNNING, &motu->states)) {
		err = start_usb_capture(motu);
		if (err < 0) {
			mutex_unlock(&motu->mutex);
			return err;
		}
	}*/

	err = start_usb_playback(motu);
	mutex_unlock(&motu->mutex);
	if (err < 0)
		return err;
		
	set_bit(ALSA_PLAYBACK_OPEN, &motu->states);

	wait_event(motu->alsa_playback_wait,
	   test_bit(CAPTURE_URB_COMPLETED, &motu->states) ||
	   !test_bit(USB_PLAYBACK_RUNNING, &motu->states));
	if (test_bit(DISCONNECTED, &motu->states))
		return -ENODEV;
	if (!test_bit(USB_PLAYBACK_RUNNING, &motu->states))
		return -EIO;
	return 0;
}

static int capture_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct motu_avb *motu = substream->private_data;
	unsigned long flags;
	struct snd_pcm_runtime *runtime;
	runtime = substream->runtime;

#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Capture trigger\n");
#endif

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (!test_bit(USB_CAPTURE_RUNNING, &motu->states))
			return -EIO;
		spin_lock_irqsave(&motu->lock, flags);
		motu->capture.buffer_pos = 0;
		motu->capture.period_pos = 0;
		set_bit(ALSA_CAPTURE_RUNNING, &motu->states);
		spin_unlock_irqrestore(&motu->lock, flags);
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
	unsigned long flags;
	struct snd_pcm_runtime *runtime;
	runtime = substream->runtime;

#ifdef DEBUG
	dev_warn(&motu->dev->dev, "Motu AVB: Playback trigger\n");
#endif

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (!test_bit(USB_PLAYBACK_RUNNING, &motu->states))
			return -EIO;
		spin_lock_irqsave(&motu->lock, flags);
		motu->playback.buffer_pos = 0;
		motu->playback.period_pos = 0;
		set_bit(ALSA_PLAYBACK_RUNNING, &motu->states);
		spin_unlock_irqrestore(&motu->lock, flags);
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
	motu->packets_per_second = 8000;
	

	if (vendor)
	{
		epd = &motu->intf[INTF_VENDOR_IN]->altsetting[1].endpoint[0].desc;
	}
	else
	{
		epd = &motu->intf[INTF_CAPTURE]->altsetting[1].endpoint[0].desc;
	}

	if (!usb_endpoint_is_isoc_in(epd)) {
		dev_err(&motu->dev->dev, "Motu AVB: Invalid capture endpoint\n");
		return -ENXIO;
	}
	motu->capture.usb_pipe = usb_rcvisocpipe(motu->dev, usb_endpoint_num(epd));
	motu->capture.max_packet_bytes = usb_endpoint_maxp_mult(epd)*usb_endpoint_maxp(epd);

    dev_warn(&motu->dev->dev, "Motu AVB: Max packets capture endpoint %d\n", motu->capture.max_packet_bytes);

	if (vendor)
	{
		epd = &motu->intf[INTF_VENDOR_OUT]->altsetting[1].endpoint[0].desc;
	}
	else
	{
		epd = &motu->intf[INTF_PLAYBACK]->altsetting[1].endpoint[0].desc;
	}

	if (!usb_endpoint_is_isoc_out(epd)) {
		dev_err(&motu->dev->dev, "Motu AVB: Invalid playback endpoint\n");
		return -ENXIO;
	}
	motu->playback.usb_pipe = usb_sndisocpipe(motu->dev, usb_endpoint_num(epd));
	motu->playback.max_packet_bytes = usb_endpoint_maxp_mult(epd)*usb_endpoint_maxp(epd);

    dev_warn(&motu->dev->dev, "Motu AVB: Max packets playback endpoint %d\n", motu->playback.max_packet_bytes);

	return 0;
}

static void free_stream_urbs(struct motu_avb_stream *stream)
{
	unsigned int urb_no;
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
	
	motu->capture.opened_count = 0;
	motu->capture.needs_prepare = true;
	motu->playback.opened_count = 0;
	motu->playback.needs_prepare = true;

    dev_info(&motu->dev->dev, "Motu AVB: midi = %d, vendor = %d\n", midi, vendor);

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
