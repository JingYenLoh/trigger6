// SPDX-License-Identifier: GPL-2.0-only

#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include <drm/drm_drv.h>

#include <sound/core.h>
#include <sound/pcm.h>

#include "trigger6.h"

#define TRIGGER6_AUDIO_RATE		48000
#define TRIGGER6_AUDIO_SILENCE_BYTES	9600
/* Half of the device's 128K, which drops samples or hangs on overflow */
#define TRIGGER6_AUDIO_BUF_SIZE		SZ_64K
/* Larger sessions also drop samples or hang the device */
#define TRIGGER6_AUDIO_SESSION_SIZE	SZ_16K

static const struct snd_pcm_hardware trigger6_pcm_hardware = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_SYNC_APPLPTR | SNDRV_PCM_INFO_NO_REWINDS,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = TRIGGER6_AUDIO_RATE,
	.rate_max = TRIGGER6_AUDIO_RATE,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = TRIGGER6_AUDIO_BUF_SIZE,
	.period_bytes_min = SZ_1K,
	.period_bytes_max = TRIGGER6_AUDIO_BUF_SIZE / 2,
	.periods_min = 2,
	.periods_max = 64,
};

/* Sends what the application has written since the last call */
static void trigger6_audio_send(struct trigger6_audio *audio)
{
	struct snd_pcm_runtime *runtime = audio->substream->runtime;
	struct trigger6_transfer *transfer;
	snd_pcm_uframes_t pending, frames, pos, first;
	u8 *buf;
	size_t len;

	pending = runtime->control->appl_ptr - audio->sent_ptr;
	if ((snd_pcm_sframes_t)pending < 0)
		pending += runtime->boundary;

	while (pending) {
		/* The timer tries again */
		transfer = trigger6_try_get_next_transfer(&audio->pool);
		if (!transfer)
			return;

		frames = min_t(snd_pcm_uframes_t, pending,
			       bytes_to_frames(runtime,
					       TRIGGER6_AUDIO_SESSION_SIZE));
		buf = transfer->data_urb->transfer_buffer;
		pos = audio->sent_ptr % runtime->buffer_size;
		first = min(frames, runtime->buffer_size - pos);
		memcpy(buf, runtime->dma_area + frames_to_bytes(runtime, pos),
		       frames_to_bytes(runtime, first));
		memcpy(buf + frames_to_bytes(runtime, first), runtime->dma_area,
		       frames_to_bytes(runtime, frames - first));

		len = frames_to_bytes(runtime, frames);
		trigger6_fill_session(transfer, TRIGGER6_SESSION_AUDIO, 0, len,
				      0, len);
		trigger6_submit_transfer(transfer, len);

		audio->sent_ptr = (audio->sent_ptr + frames) % runtime->boundary;
		audio->frames_sent += frames;
		pending -= frames;
	}
}

static void trigger6_audio_send_silence(struct trigger6_audio *audio)
{
	struct trigger6_transfer *transfer;

	transfer = trigger6_try_get_next_transfer(&audio->pool);
	if (!transfer)
		return;

	memset(transfer->data_urb->transfer_buffer, 0,
	       TRIGGER6_AUDIO_SILENCE_BYTES);
	trigger6_fill_session(transfer, TRIGGER6_SESSION_AUDIO, 0,
			      TRIGGER6_AUDIO_SILENCE_BYTES, 0,
			      TRIGGER6_AUDIO_SILENCE_BYTES);
	trigger6_submit_transfer(transfer, TRIGGER6_AUDIO_SILENCE_BYTES);
}

/* The device has no position feedback, so the pointer follows the clock */
static enum hrtimer_restart trigger6_audio_timer(struct hrtimer *timer)
{
	struct trigger6_audio *audio =
		container_of(timer, struct trigger6_audio, timer);
	struct snd_pcm_substream *substream = audio->substream;
	unsigned long flags;
	bool running;

	snd_pcm_stream_lock_irqsave(substream, flags);
	running = audio->running;
	if (running)
		trigger6_audio_send(audio);
	snd_pcm_stream_unlock_irqrestore(substream, flags);

	if (!running)
		return HRTIMER_NORESTART;

	snd_pcm_period_elapsed(substream);
	hrtimer_forward_now(timer, audio->period);

	return HRTIMER_RESTART;
}

static int trigger6_pcm_open(struct snd_pcm_substream *substream)
{
	struct trigger6_device *trigger6 = snd_pcm_substream_chip(substream);

	substream->runtime->hw = trigger6_pcm_hardware;
	trigger6->audio.substream = substream;

	return 0;
}

static int trigger6_pcm_close(struct snd_pcm_substream *substream)
{
	struct trigger6_device *trigger6 = snd_pcm_substream_chip(substream);

	trigger6->audio.substream = NULL;

	return 0;
}

static int trigger6_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct trigger6_device *trigger6 = snd_pcm_substream_chip(substream);
	struct trigger6_audio *audio = &trigger6->audio;
	int idx;

	if (audio->enabled && drm_dev_enter(&trigger6->drm, &idx)) {
		trigger6_set_audio(trigger6, false);
		drm_dev_exit(idx);
	}
	audio->enabled = false;

	return 0;
}

static int trigger6_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct trigger6_device *trigger6 = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct trigger6_audio *audio = &trigger6->audio;
	int idx, ret;

	audio->period = ns_to_ktime(div_u64((u64)runtime->period_size *
					    NSEC_PER_SEC, runtime->rate));
	audio->sent_ptr = 0;
	audio->frames_sent = 0;

	if (audio->enabled)
		return 0;

	if (!drm_dev_enter(&trigger6->drm, &idx))
		return -ENODEV;

	ret = trigger6_set_audio(trigger6, true);
	drm_dev_exit(idx);
	if (ret)
		return ret;

	audio->enabled = true;

	return 0;
}

static int trigger6_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct trigger6_device *trigger6 = snd_pcm_substream_chip(substream);
	struct trigger6_audio *audio = &trigger6->audio;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		audio->running = true;
		audio->start = ktime_get();
		trigger6_audio_send(audio);
		hrtimer_start(&audio->timer, audio->period,
			      HRTIMER_MODE_REL_SOFT);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		audio->running = false;
		hrtimer_try_to_cancel(&audio->timer);
		/* Sinks may hold the last sample */
		trigger6_audio_send_silence(audio);
		return 0;
	default:
		return -EINVAL;
	}
}

static int trigger6_pcm_sync_stop(struct snd_pcm_substream *substream)
{
	struct trigger6_device *trigger6 = snd_pcm_substream_chip(substream);

	hrtimer_cancel(&trigger6->audio.timer);

	return 0;
}

static snd_pcm_uframes_t
trigger6_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct trigger6_device *trigger6 = snd_pcm_substream_chip(substream);
	struct trigger6_audio *audio = &trigger6->audio;
	s64 elapsed = ktime_to_ns(ktime_sub(ktime_get(), audio->start));
	u32 pos;
	u64 played;

	played = mul_u64_u32_div(elapsed, TRIGGER6_AUDIO_RATE, NSEC_PER_SEC);
	played = min(played, audio->frames_sent);
	div_u64_rem(played, substream->runtime->buffer_size, &pos);

	return pos;
}

/* Sends the samples as soon as they are written */
static int trigger6_pcm_ack(struct snd_pcm_substream *substream)
{
	struct trigger6_device *trigger6 = snd_pcm_substream_chip(substream);

	if (trigger6->audio.running)
		trigger6_audio_send(&trigger6->audio);

	return 0;
}

static const struct snd_pcm_ops trigger6_pcm_ops = {
	.open = trigger6_pcm_open,
	.close = trigger6_pcm_close,
	.hw_free = trigger6_pcm_hw_free,
	.prepare = trigger6_pcm_prepare,
	.trigger = trigger6_pcm_trigger,
	.sync_stop = trigger6_pcm_sync_stop,
	.pointer = trigger6_pcm_pointer,
	.ack = trigger6_pcm_ack,
};

/* The card can outlive the USB device while its files are still open */
static void trigger6_audio_card_free(struct snd_card *card)
{
	struct trigger6_device *trigger6 = card->private_data;

	drm_dev_put(&trigger6->drm);
}

int trigger6_audio_init(struct trigger6_device *trigger6)
{
	struct trigger6_audio *audio = &trigger6->audio;
	struct usb_device *udev = interface_to_usbdev(trigger6->intf);
	struct snd_card *card;
	struct snd_pcm *pcm;
	int ret;

	hrtimer_setup(&audio->timer, trigger6_audio_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);

	ret = trigger6_alloc_pool(trigger6, &audio->pool,
				  TRIGGER6_AUDIO_SESSION_SIZE);
	if (ret)
		goto err_free_pool;

	ret = snd_card_new(&trigger6->intf->dev, -1, "Trigger6", THIS_MODULE, 0,
			   &card);
	if (ret)
		goto err_free_pool;

	drm_dev_get(&trigger6->drm);
	card->private_data = trigger6;
	card->private_free = trigger6_audio_card_free;

	strscpy(card->driver, DRIVER_NAME);
	strscpy(card->shortname, "MCT Trigger 6");
	snprintf(card->longname, sizeof(card->longname), "%s at %s",
		 DRIVER_DESC, dev_name(&udev->dev));

	ret = snd_pcm_new(card, "HDMI", 0, 1, 0, &pcm);
	if (ret)
		goto err_free_card;

	pcm->private_data = trigger6;
	strscpy(pcm->name, "HDMI");
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &trigger6_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	ret = snd_card_register(card);
	if (ret)
		goto err_free_card;

	audio->card = card;
	audio->pcm = pcm;

	return 0;

err_free_card:
	snd_card_free(card);
err_free_pool:
	trigger6_free_pool(&audio->pool);
	return ret;
}

void trigger6_audio_disconnect(struct trigger6_device *trigger6)
{
	struct trigger6_audio *audio = &trigger6->audio;

	snd_card_disconnect(audio->card);
	hrtimer_cancel(&audio->timer);
	trigger6_kill_pool(&audio->pool);
	trigger6_free_pool(&audio->pool);
	snd_card_free_when_closed(audio->card);
}

void trigger6_audio_suspend(struct trigger6_device *trigger6)
{
	struct trigger6_audio *audio = &trigger6->audio;

	snd_power_change_state(audio->card, SNDRV_CTL_POWER_D3hot);
	snd_pcm_suspend_all(audio->pcm);
	hrtimer_cancel(&audio->timer);
	trigger6_kill_pool(&audio->pool);
	/* Enabled again by the prepare that follows the resume */
	audio->enabled = false;
}

void trigger6_audio_resume(struct trigger6_device *trigger6)
{
	snd_power_change_state(trigger6->audio.card, SNDRV_CTL_POWER_D0);
}
