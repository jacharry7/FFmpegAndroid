/*
 * Copyright (c) 2012 Jacek Marchwicki
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include <libavutil/avstring.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>

#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

//#include <libavcodec/opt.h>
#include <libavcodec/avfft.h>

#include <android/bitmap.h>
#include <android/log.h>

#include <jni.h>
#include <pthread.h>

#ifdef PROFILER
#include <android-ndk-profiler-3.1/prof.h>
#endif

#ifdef YUV2RGB
#include <yuv2rgb/yuv2rgb.h>
#endif

/*local headers*/
#include "helpers.h"
#include "queue.h"
#include "player.h"
#include "jni-protocol.h"
#include "aes-protocol.h"

#include "player_static.h"

#define LOG_LEVEL 1
#define LOG_TAG "AVEngine:player.c"
#define LOGI(level, ...) if (level <= LOG_LEVEL) {__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__);}
#define LOGE(level, ...) if (level <= LOG_LEVEL) {__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__);}
#define LOGW(level, ...) if (level <= LOG_LEVEL) {__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__);}

#define FALSE 0
#define TRUE (!(FALSE))

#define DO_NOT_SEEK (0xdeadbeef)

#define MIN_SLEEP_TIME_MS 2
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

//#define MEASURE_TIME

typedef struct Player {
	JavaVM *get_javavm;

	jclass player_class;
	jclass audio_track_class;
	jmethodID audio_track_write;
	jmethodID audio_track_play;
	jmethodID audio_track_pause;
	jmethodID audio_track_flush;
	jmethodID audio_track_stop;
	jmethodID audio_track_getChannelCount;
	jmethodID audio_track_getSampleRate;

	jmethodID player_prepareFrame;
	jmethodID player_onUpdateTime;
	jmethodID player_prepareAudioTrack;

	pthread_mutex_t mutex_operation;

	AVFormatContext *format_ctx;
	int input_inited;
	int64_t open_time;

	int video_index;
	int audio_index;
	AVStream *input_streams[AVMEDIA_TYPE_NB];
	AVCodecContext *input_codec_ctxs[AVMEDIA_TYPE_NB];
	int stream_indexs[AVMEDIA_TYPE_NB];
	AVFrame *input_frames[AVMEDIA_TYPE_NB];

	enum PixelFormat out_format;

	jobject audio_track;
	enum AVSampleFormat audio_track_format;
	int audio_track_channel_count;

	struct SwsContext *sws_context;
	struct SwrContext *swr_context;
	DECLARE_ALIGNED(16,uint8_t,audio_buf2)[AVCODEC_MAX_AUDIO_FRAME_SIZE * 4];

	long video_duration;
	int last_updated_time;

	int playing;

	pthread_mutex_t mutex_queue;
	pthread_cond_t cond_queue;
	Queue *packets_queue[AVMEDIA_TYPE_NB];
	Queue *rgb_video_queue;

	int interrupt_renderer;
	int pause;
	int stop;
	int seek_position;
	int flush_streams[AVMEDIA_TYPE_NB];
	int flush_video_play;

	int stop_streams[AVMEDIA_TYPE_NB];

	int rendering;

	pthread_t read_stream_thread;
	pthread_t decode_threads[AVMEDIA_TYPE_NB];

	int read_stream_thread_created;
	int decode_threads_created[AVMEDIA_TYPE_NB];

	double audio_clock;
	int64_t audio_write_time;

	int64_t audio_pause_time;
	int64_t audio_resume_time;

	double video_current_pts;       // current displayed pts (different from video_clock if frame fifos are used)
	double video_current_pts_drift; // video_current_pts - time (av_gettime) at which we updated video_current_pts - used to have running video pts
	double external_clock;          ///< external clock base
	double external_clock_drift;    ///< external clock base - time (av_gettime) at which we updated external_clock
	int64_t external_clock_time;    ///< last reference time
	double external_clock_speed;    ///< speed of the external clock

#ifdef YUV2RGB
	int dither;
#endif
} Player;

typedef struct State {
	Player *player;
	JNIEnv* env;
	jobject thiz;
} State;

typedef struct DecoderState {
	int stream_index;
	enum AVMediaType stream_type;
	struct Player *player;
	JNIEnv* env;
	jobject thiz;
} DecoderState;

typedef struct DecoderData {
	struct Player *player;
	enum AVMediaType stream_type;
} DecoderData;

typedef struct VideoRGBFrameElem {
	AVFrame *frame;
	jobject jbitmap;
	double time;
	int end_of_stream;
} VideoRGBFrameElem;

typedef struct PacketData {
	int end_of_stream;
	AVPacket *packet;
} PacketData;

void throw_exception(JNIEnv *env, const char * exception_class_path,
		const char *msg) {
	jclass newExcCls = (*env)->FindClass(env, exception_class_path);
	if (newExcCls == NULL) {
		assert(FALSE);
	}
	(*env)->ThrowNew(env, newExcCls, msg);
	(*env)->DeleteLocalRef(env, newExcCls);
}

void throw_interrupted_exception(JNIEnv *env, const char * msg) {
	throw_exception(env, interrupted_exception_class_path, msg);
}

void throw_runtime_exception(JNIEnv *env, const char * msg) {
	throw_exception(env, runtime_exception_class_path, msg);
}

/* get the current video clock value */
static double get_video_clock(Player *player) {
	if (player->pause) {
		return player->video_current_pts;
	} else {
		return player->video_current_pts_drift + av_gettime() / 1000000.0;
	}
}

/* get the current external clock value */
static double get_external_clock(Player *player) {
	if (player->pause) {
		return player->external_clock;
	} else {
		double time = av_gettime() / 1000000.0;
		return player->external_clock_drift + time - (time - player->external_clock_time / 1000000.0) * (1.0 - player->external_clock_speed);
	}
}

static void update_external_clock_pts(Player *player, double pts) {
	player->external_clock_time = av_gettime();
	player->external_clock = pts;
	player->external_clock_drift = pts - player->external_clock_time / 1000000.0;
}

static void update_external_clock_speed(Player *player, double speed) {
	update_external_clock_pts(player, get_external_clock(player));
	player->external_clock_speed = speed;
}

#if 0
static void check_external_clock_speed(Player *player) {
	double speed = player->external_clock_speed;
	if (speed != 1.0)
		update_external_clock_speed(player, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
}
#endif

static void update_video_pts(Player *player, double pts) {
	double time = av_gettime() / 1000000.0;
	/* update current video pts */
	player->video_current_pts = pts;
	player->video_current_pts_drift = player->video_current_pts - time;
}

int player_write_audio(DecoderData *decoder_data, JNIEnv *env,
	int64_t pts, uint8_t *data, int data_size, int original_data_size) {
	Player *player = decoder_data->player;
	int err = ERROR_NO_ERROR;
	int ret;
	AVCodecContext *ctx = player->input_codec_ctxs[AVMEDIA_TYPE_AUDIO];
	AVStream *stream = player->input_streams[AVMEDIA_TYPE_AUDIO];
	LOGI(10, "player_write_audio Writing audio frame")

	jbyteArray samples_byte_array = (*env)->NewByteArray(env, data_size);
	if (samples_byte_array == NULL) {
		err = -ERROR_NOT_CREATED_AUDIO_SAMPLE_BYTE_ARRAY;
		goto end;
	}

	pthread_mutex_lock(&player->mutex_queue);

	if (pts != AV_NOPTS_VALUE) {
		player->audio_clock = av_q2d(stream->time_base) * pts;
	} else {
		player->audio_clock += (double) original_data_size
				/ (ctx->channels * ctx->sample_rate
						* av_get_bytes_per_sample(ctx->sample_fmt));
	}
	player->audio_write_time = av_gettime();
	pthread_cond_broadcast(&player->cond_queue);
	pthread_mutex_unlock(&player->mutex_queue);

	LOGI(10, "player_write_audio Writing sample data")

	jbyte *jni_samples = (*env)->GetByteArrayElements(env, samples_byte_array,
			NULL);
	memcpy(jni_samples, data, data_size);
	(*env)->ReleaseByteArrayElements(env, samples_byte_array, jni_samples, 0);

	LOGI(10, "player_write_audio playing audio track");
	ret = (*env)->CallIntMethod(env, player->audio_track,
			player->audio_track_write, samples_byte_array, 0, data_size);
	jthrowable exc = (*env)->ExceptionOccurred(env);
	if (exc) {
		err = -ERROR_PLAYING_AUDIO;
		LOGE(3, "Could not write audio track: reason in exception");
		goto free_local_ref;
	}
	if (ret < 0) {
		err = -ERROR_PLAYING_AUDIO;
		LOGE(3,
				"Could not write audio track: reason: %d look in AudioTrack.write()", ret);
		goto free_local_ref;
	}

free_local_ref:
	LOGI(10, "player_write_audio releasing local ref");
	(*env)->DeleteLocalRef(env, samples_byte_array);
end:
	return err;
}

QueueCheckFuncRet player_decode_queue_check(Queue *queue, DecoderData *decoderData, int *ret) {
	Player *player = decoderData->player;

	if (player->stop_streams[decoderData->stream_type]) {
		*ret = DECODE_CHECK_MSG_STOP;
		return QUEUE_CHECK_FUNC_RET_SKIP;
	}
	if (player->flush_streams[decoderData->stream_type]) {
		*ret = DECODE_CHECK_MSG_FLUSH;
		return QUEUE_CHECK_FUNC_RET_SKIP;
	}
	return QUEUE_CHECK_FUNC_RET_TEST;
}

int player_decode_audio(DecoderData * decoder_data, JNIEnv * env, PacketData *packet_data) {
	int got_frame_ptr;
	Player *player = decoder_data->player;
	AVCodecContext *ctx = player->input_codec_ctxs[AVMEDIA_TYPE_AUDIO];
	AVFrame *frame = player->input_frames[AVMEDIA_TYPE_AUDIO];

	LOGI(10, "player_decode_audio decoding");
	AVPacket *packet = packet_data->packet;
	int len = avcodec_decode_audio4(ctx, frame, &got_frame_ptr, packet);
	if (len < 0) {
		LOGE(1, "Fail decoding audio %d\n", len);
		return -ERROR_WHILE_DECODING_AUDIO_FRAME;
	}
	if (!got_frame_ptr) {
		LOGI(3, "player_decode_audio Audio frame not finished\n");
		return 0;
	}

	int64_t pts = packet->pts;
	int original_data_size = av_samples_get_buffer_size(NULL, ctx->channels,
			frame->nb_samples, ctx->sample_fmt, 1);
	uint8_t *audio_buf;
	int data_size;

	if (player->swr_context != NULL) {
		uint8_t *out[] = { player->audio_buf2 };
		int sample_per_buffer_divider = player->audio_track_channel_count
				* av_get_bytes_per_sample(player->audio_track_format);
		int len2 = swr_convert(player->swr_context, out,
				sizeof(player->audio_buf2) / sample_per_buffer_divider,
				(uint8_t const **)frame->data, frame->nb_samples);
		if (len2 < 0) {
			LOGE(1, "Could not resample frame");
			return -ERROR_COULD_NOT_RESAMPLE_FRAME;
		}
		if (len2 == sizeof(player->audio_buf2) / sample_per_buffer_divider) {
			LOGI(1, "warning: audio buffer is probably too small\n");
			swr_init(player->swr_context);
		}
		audio_buf = player->audio_buf2;
		data_size = len2 * sample_per_buffer_divider;
	} else {
		audio_buf = frame->data[0];
		data_size = original_data_size;
	}

	LOGI(10, "player_decode_audio Decoded audio frame\n");

	int err;
	if ((err = player_write_audio(decoder_data, env, pts, audio_buf, data_size,
			original_data_size))) {
		LOGE(1, "Could not write frame");
		return err;
	}
	return 0;
}

int player_decode_video(DecoderData * decoder_data, JNIEnv * env, PacketData *packet_data) {
	Player *player = decoder_data->player;
	AVCodecContext *ctx = player->input_codec_ctxs[AVMEDIA_TYPE_VIDEO];
	AVFrame *frame = player->input_frames[AVMEDIA_TYPE_VIDEO];
	AVStream *stream = player->input_streams[AVMEDIA_TYPE_VIDEO];
	int interrupt_ret;
	int to_write;
	VideoRGBFrameElem * elem;

	if (packet_data->end_of_stream) {
		LOGI(2, "player_decode_video waiting for queue to end of stream");
		pthread_mutex_lock(&player->mutex_queue);
		elem = queue_push_start_impl(player->rgb_video_queue,
			&player->mutex_queue, &player->cond_queue, &to_write,
			(QueueCheckFunc) player_decode_queue_check, decoder_data,
			(void **) &interrupt_ret);
		if (elem == NULL) {
			if (interrupt_ret == DECODE_CHECK_MSG_STOP) {
				LOGI(2, "player_decode_video push stop");
			} else if (interrupt_ret == DECODE_CHECK_MSG_FLUSH) {
				LOGI(2, "player_decode_video push flush");
			} else {
				assert(FALSE);
			}
			pthread_mutex_unlock(&player->mutex_queue);
			return 0;
		}
		elem->end_of_stream = TRUE;
		LOGI(2, "player_decode_video sending end of stream");
		queue_push_finish_impl(player->rgb_video_queue,
			&player->mutex_queue, &player->cond_queue, to_write);
		pthread_mutex_unlock(&player->mutex_queue);
		return 0;
	}

	LOGI(10, "player_decode_video decoding");
	int frameFinished;
	int ret = avcodec_decode_video2(ctx, frame, &frameFinished, packet_data->packet);
	if (ret < 0) {
		LOGE(1, "player_decode_video Fail decoding video %d\n", ret);
		return -ERROR_WHILE_DECODING_VIDEO;
	}
	if (!frameFinished) {
		LOGI(10, "player_decode_video Video frame not finished\n");
		return 0;
	}

	int64_t pts = av_frame_get_best_effort_timestamp(frame);
	if (pts == AV_NOPTS_VALUE) {
		pts = 0;
	}

	double time = (double) pts * av_q2d(stream->time_base);
	LOGI(10,
			"player_decode_video Decoded video frame: %f, time_base: %lld", time, pts);

	LOGI(7, "player_decode_video copy wait");

	pthread_mutex_lock(&player->mutex_queue);
	elem = queue_push_start_impl(player->rgb_video_queue,
		&player->mutex_queue, &player->cond_queue, &to_write,
		(QueueCheckFunc) player_decode_queue_check, decoder_data,
		(void **) &interrupt_ret);
	if (elem == NULL) {
		if (interrupt_ret == DECODE_CHECK_MSG_STOP) {
			LOGI(2, "player_decode_video push stop");
		} else if (interrupt_ret == DECODE_CHECK_MSG_FLUSH) {
			LOGI(2, "player_decode_video push flush");
		} else {
			assert(FALSE);
		}
		pthread_mutex_unlock(&player->mutex_queue);
		return 0;
	}

	pthread_mutex_unlock(&player->mutex_queue);
	elem->time = time;
	elem->end_of_stream = FALSE;
	AVFrame * rgbFrame = elem->frame;
	void *buffer;
	int destWidth = ctx->width;
	int destHeight = ctx->height;
	int err = 0;

	if ((ret = AndroidBitmap_lockPixels(env, elem->jbitmap, &buffer)) < 0) {
		LOGE(1, "AndroidBitmap_lockPixels() failed ! error=%d", ret);
		err = -ERROR_WHILE_LOCING_BITMAP;
		goto fail_lock_bitmap;
	}

	avpicture_fill((AVPicture *) elem->frame, buffer, player->out_format,
			destWidth, destHeight);

	LOGI(7, "player_decode_video copying...");
#ifdef YUV2RGB
	if (ctx->pix_fmt == AV_PIX_FMT_YUV420P) {
		LOGI(9, "Using yuv420_2_rgb565");
		yuv420_2_rgb565(rgbFrame->data[0], frame->data[0], frame->data[1],
			frame->data[2], destWidth, destHeight, frame->linesize[0],
			frame->linesize[1], destWidth << 1, yuv2rgb565_table,
			player->dither++);
	} else if (ctx->pix_fmt == AV_PIX_FMT_NV12) {
		LOGI(9, "Using nv12_2_rgb565");
		nv12_2_rgb565(rgbFrame->data[0], frame->data[0], frame->data[1],
			frame->data[1]+1, destWidth, destHeight, frame->linesize[0],
			frame->linesize[1], destWidth << 1, yuv2rgb565_table,
			player->dither++);
	} else
#endif
	{
		LOGI(9, "Using sws_scale");
		sws_scale(player->sws_context,
				(const uint8_t * const *) frame->data,
				frame->linesize, 0, ctx->height,
				rgbFrame->data, rgbFrame->linesize);
	}

	AndroidBitmap_unlockPixels(env, elem->jbitmap);

fail_lock_bitmap:
	queue_push_finish(player->rgb_video_queue, &player->mutex_queue,
		&player->cond_queue, to_write);
	return err;
}

void *player_decode(void * data) {
	int err = ERROR_NO_ERROR;
	DecoderData *decoder_data = data;
	Player *player = decoder_data->player;
	Queue *queue = player->packets_queue[decoder_data->stream_type];
	AVCodecContext *ctx = player->input_codec_ctxs[decoder_data->stream_type];
	enum AVMediaType codec_type = ctx->codec_type;

	int stop = FALSE;
	JNIEnv * env;
	char thread_title[256];
	sprintf(thread_title, "FFmpegDecode[%d]", decoder_data->stream_type);

	JavaVMAttachArgs thread_spec = { JNI_VERSION_1_4, thread_title, NULL };

	jint ret = (*player->get_javavm)->AttachCurrentThread(player->get_javavm,
			&env, &thread_spec);
	if (ret || env == NULL) {
		err = -ERROR_COULD_NOT_ATTACH_THREAD;
		goto end;
	}

	for (;;) {
		LOGI(10, "player_decode[%d] waiting for frame", decoder_data->stream_type);
		int interrupt_ret;
		PacketData *packet_data;
		int has_sleep;
		pthread_mutex_lock(&player->mutex_queue);
pop:
		has_sleep = 0;
		interrupt_ret = -1;
		while (player->pause && !player->stop) {
			// we try to sleep 10ms
			if (!has_sleep) {
				LOGI(3, "player_decode[%d] enter sleep...", decoder_data->stream_type);
				has_sleep = 1;
			}
			player_decode_queue_check(queue, decoder_data, &interrupt_ret);
			// MUST wake up from PAUSE --> SEEK/STOP
			if (interrupt_ret == DECODE_CHECK_MSG_FLUSH) {
				LOGI(2, "player_decode[%d] interrupted by FLUSH from PAUSE", decoder_data->stream_type);
				goto flush;
			} else if (interrupt_ret == DECODE_CHECK_MSG_STOP) {
				LOGI(2, "player_decode[%d] interrupted by STOP from PAUSE", decoder_data->stream_type);
				goto stop;
			}
			pthread_cond_timeout_np(&player->cond_queue, &player->mutex_queue, 10);
		}
		if (has_sleep)
			LOGI(3, "player_decode[%d] wake up...", decoder_data->stream_type);

		packet_data = queue_pop_start_impl(&queue,
			&player->mutex_queue, &player->cond_queue,
			(QueueCheckFunc) player_decode_queue_check, decoder_data,
			(void **) &interrupt_ret);
		if (packet_data == NULL) {
			if (interrupt_ret == DECODE_CHECK_MSG_FLUSH) {
				LOGI(3, "player_decode[%d] interrupted by FLUSH", decoder_data->stream_type);
				goto flush;
			} else if (interrupt_ret == DECODE_CHECK_MSG_STOP) {
				LOGI(3, "player_decode[%d] interrupted by STOP", decoder_data->stream_type);
				goto stop;
			} else {
				assert(FALSE);
			}
		}
		pthread_mutex_unlock(&player->mutex_queue);
		LOGI(10, "player_decode[%d] decoding frame", decoder_data->stream_type);
		if (packet_data->end_of_stream) {
			LOGI(10, "player_decode[%d] read end of stream", decoder_data->stream_type);
		}

		if (codec_type == AVMEDIA_TYPE_AUDIO) {
			err = player_decode_audio(decoder_data, env, packet_data);
		} else if (codec_type == AVMEDIA_TYPE_VIDEO) {
			err = player_decode_video(decoder_data, env, packet_data);
		}

		if (!packet_data->end_of_stream) {
			av_free_packet(packet_data->packet);
		}
		queue_pop_finish(queue, &player->mutex_queue, &player->cond_queue);
		if (err < 0) {
			pthread_mutex_lock(&player->mutex_queue);
			goto stop;
		}
		continue;
stop:
		LOGI(2, "player_decode[%d] stop", decoder_data->stream_type);
		stop = TRUE;
flush:
		LOGI(2, "player_decode[%d] flush", decoder_data->stream_type);
		PacketData *to_free; // FIXME move to PacketData
		while ((to_free = queue_pop_start_impl_non_block(queue))
				!= NULL) {
			if (!to_free->end_of_stream) {
				av_free_packet(to_free->packet);
			}
			queue_pop_finish_impl(queue, &player->mutex_queue, &player->cond_queue);
		}
		LOGI(2, "player_decode[%d] flushing", decoder_data->stream_type);

		if (codec_type == AVMEDIA_TYPE_AUDIO) {
			(*env)->CallVoidMethod(env, player->audio_track, player->audio_track_flush);
		} else if (codec_type == AVMEDIA_TYPE_VIDEO) {
			if (!player->rendering) {
				LOGI(2, "player_decode_video not rendering flushing rgb_video_queue");
				VideoRGBFrameElem *elem;
				while ((elem = queue_pop_start_impl_non_block(
						player->rgb_video_queue)) != NULL) {
					queue_pop_finish_impl(player->rgb_video_queue, &player->mutex_queue, &player->cond_queue);
				}
			} else {
				LOGI(2,
						"player_decode_video rendering sending rgb_video_queue flush request");
				player->flush_video_play = TRUE;
				pthread_cond_broadcast(&player->cond_queue);
				LOGI(2, "player_decode_video waiting for rgb_video_queue flush");
				while (player->flush_video_play)
					pthread_cond_wait(&player->cond_queue, &player->mutex_queue);
			}
		}
		LOGI(2, "player_decode[%d] flushed", decoder_data->stream_type);

		if (stop) {
			LOGI(2, "player_decode[%d] signal stop", decoder_data->stream_type);
			player->stop_streams[decoder_data->stream_type] = FALSE;
			pthread_cond_broadcast(&player->cond_queue);
			pthread_mutex_unlock(&player->mutex_queue);
			goto detach_current_thread;
		} else {
			LOGI(2, "player_decode[%d] signal flush", decoder_data->stream_type);
			player->flush_streams[decoder_data->stream_type] = FALSE;
			pthread_cond_broadcast(&player->cond_queue);
			goto pop;
		}
	}

detach_current_thread:
	ret = (*player->get_javavm)->DetachCurrentThread(player->get_javavm);
	if (ret && !err)
		err = ERROR_COULD_NOT_DETACH_THREAD;

end:
	free(decoder_data);
	decoder_data = NULL;

	return NULL;
}

QueueCheckFuncRet player_read_stream_check(Queue *queue, Player *player, int *ret) {
	if (player->stop) {
		*ret = READ_FROM_STREAM_CHECK_MSG_STOP;
		return QUEUE_CHECK_FUNC_RET_SKIP;
	}
	if (player->seek_position != DO_NOT_SEEK) {
		*ret = READ_FROM_STREAM_CHECK_MSG_SEEK;
		return QUEUE_CHECK_FUNC_RET_SKIP;
	}
	return QUEUE_CHECK_FUNC_RET_TEST;
}

static void player_assign_to_no_boolean_array(Player *player, int* array, int value) {
	int i;
	for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
		if (player->input_codec_ctxs[i])
			array[i] = value;
	}
}
static int player_if_all_no_array_elements_has_value(Player *player, int *array, int value) {
	int i;
	for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
		if (player->input_codec_ctxs[i] && array[i] != value)
			return FALSE;
	}
	return TRUE;
}

void * player_read_stream(void *data) {
	Player *player = (Player *) data;
	int i, err = ERROR_NO_ERROR;
	AVPacket packet, *pkt = &packet;
	int64_t seek_target;
	JNIEnv *env;
	Queue *queue;
	int seek_stream_index;
	AVStream *seek_stream;
	PacketData *packet_data;
	int to_write;
	int interrupt_ret;
	JavaVMAttachArgs thread_spec = { JNI_VERSION_1_4, "FFmpegReadStream", NULL };

	jint ret = (*player->get_javavm)->AttachCurrentThread(player->get_javavm, &env, &thread_spec);
	if (ret) {
		err = ERROR_COULD_NOT_ATTACH_THREAD;
		goto end;
	}

	// MUST initialize it
	av_init_packet(pkt);
	for (;;) {
		while (player->pause && !player->stop) {
			usleep(10000);
			// MUST wake up from PAUSE --> SEEK/STOP
			if (player->seek_position != DO_NOT_SEEK) {
				av_init_packet(pkt);
				goto seek_loop;
			}
		}
		int ret = av_read_frame(player->format_ctx, pkt);
		if (ret < 0) {
			pthread_mutex_lock(&player->mutex_queue);
			LOGI(3, "player_read_stream stream end");
			queue = player->packets_queue[AVMEDIA_TYPE_VIDEO];
			packet_data = queue_push_start_impl(queue,
				&player->mutex_queue, &player->cond_queue, &to_write,
				(QueueCheckFunc) player_read_stream_check, player,
				(void **)&interrupt_ret);
			if (packet_data == NULL) {
				if (interrupt_ret == READ_FROM_STREAM_CHECK_MSG_STOP) {
					LOGI(2, "player_read_stream queue interrupt stop");
					goto exit_loop;
				} else if (interrupt_ret == READ_FROM_STREAM_CHECK_MSG_SEEK) {
					LOGI(2, "player_read_stream queue interrupt seek");
					av_init_packet(pkt);
					goto seek_loop;
				} else {
					assert(FALSE);
				}
			}
			packet_data->end_of_stream = TRUE;
			LOGI(3, "player_read_stream sending end_of_stream packet");
			queue_push_finish_impl(queue, &player->mutex_queue, &player->cond_queue, to_write);
			for (;;) {
				if (player->stop)
					goto exit_loop;
				if (player->seek_position != DO_NOT_SEEK) {
					av_init_packet(pkt);
					goto seek_loop;
				}
				pthread_cond_wait(&player->cond_queue, &player->mutex_queue);
			}
			pthread_mutex_unlock(&player->mutex_queue);
		}

		pthread_mutex_lock(&player->mutex_queue);
		if (player->stop) {
			LOGI(4, "player_read_stream stopping");
			goto exit_loop;
		}
		if (player->seek_position != DO_NOT_SEEK) {
			goto seek_loop;
		}

parse_frame:
		queue = NULL;
		LOGI(10, "player_read_stream looking for stream")
		for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
			if (packet.stream_index == player->stream_indexs[i]) {
				queue = player->packets_queue[i];
				LOGI(10, "player_read_stream stream found [%d]", i);
				break;
			}
		}

		if (queue == NULL) {
			LOGI(2, "player_read_stream stream not found");
			goto skip_loop;
		}

		LOGI(10, "player_read_stream waiting for queue");
		packet_data = queue_push_start_impl(queue,
			&player->mutex_queue, &player->cond_queue, &to_write,
			(QueueCheckFunc) player_read_stream_check, player,
			(void **)&interrupt_ret);
		if (packet_data == NULL) {
			if (interrupt_ret == READ_FROM_STREAM_CHECK_MSG_STOP) {
				LOGI(2, "player_read_stream queue interrupt stop");
				goto exit_loop;
			} else if (interrupt_ret == READ_FROM_STREAM_CHECK_MSG_SEEK) {
				LOGI(2, "player_read_stream queue interrupt seek");
				goto seek_loop;
			} else {
				assert(FALSE);
			}
		}

		pthread_mutex_unlock(&player->mutex_queue);
		packet_data->end_of_stream = FALSE;
		*packet_data->packet = packet;

		if (av_dup_packet(packet_data->packet) < 0) {
			err = ERROR_WHILE_DUPLICATING_FRAME;
			pthread_mutex_lock(&player->mutex_queue);
			goto exit_loop;
		}

		queue_push_finish(queue, &player->mutex_queue, &player->cond_queue, to_write);
		continue;

exit_loop:
		LOGI(3, "player_read_stream stop");
		av_free_packet(pkt);

		//request stream to stop
		player_assign_to_no_boolean_array(player, player->stop_streams, TRUE);
		pthread_cond_broadcast(&player->cond_queue);

		// wait for all stream stop
		while (!player_if_all_no_array_elements_has_value(player,
				player->stop_streams, FALSE))
			pthread_cond_wait(&player->cond_queue, &player->mutex_queue);
		LOGI(3, "player_read_stream stopped");

		for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
			if (player->input_codec_ctxs[i])
				avcodec_flush_buffers(player->input_codec_ctxs[i]);
		}

		pthread_mutex_unlock(&player->mutex_queue);
		goto detach_current_thread;

seek_loop:
		seek_stream_index = player->stream_indexs[AVMEDIA_TYPE_VIDEO];
		seek_stream = player->input_streams[AVMEDIA_TYPE_VIDEO];

		// getting seek target time in time_base value
		seek_target = av_rescale_q(AV_TIME_BASE * (int64_t) player->seek_position, AV_TIME_BASE_Q,
			seek_stream->time_base);
		LOGI(3, "player_read_stream seeking to: %ds, time_base: %lld", player->seek_position, seek_target);

		if (av_seek_frame(player->format_ctx, seek_stream_index, seek_target, 0) < 0) {
			// seeking error - trying to play movie without it
			LOGE(1, "Error while seeking");
			player->seek_position = DO_NOT_SEEK;
			pthread_cond_broadcast(&player->cond_queue);
			goto parse_frame;
		}

		LOGI(3, "player_read_stream seeking success");

		// request stream to flush
		player_assign_to_no_boolean_array(player, player->flush_streams, TRUE);
		LOGI(3, "player_read_stream flushing audio")
		// flush audio buffer
		if (player->audio_track) {
			(*env)->CallVoidMethod(env, player->audio_track, player->audio_track_flush);
		}
		LOGI(3, "player_read_stream flushed audio");
		pthread_cond_broadcast(&player->cond_queue);

		LOGI(3, "player_read_stream waiting for flush");

		// waiting for all stream flush
		while (!player_if_all_no_array_elements_has_value(player,
				player->flush_streams, FALSE))
			pthread_cond_wait(&player->cond_queue, &player->mutex_queue);

		LOGI(3, "player_read_stream flushing internal codec buffers");
		for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
			if (player->input_codec_ctxs[i])
				avcodec_flush_buffers(player->input_codec_ctxs[i]);
		}

		player->seek_position = DO_NOT_SEEK;
		update_external_clock_pts(player, seek_target / (double)AV_TIME_BASE);
		pthread_cond_broadcast(&player->cond_queue);
		LOGI(3, "player_read_stream ending seek");

skip_loop:
		av_free_packet(pkt);
		pthread_mutex_unlock(&player->mutex_queue);
	}

detach_current_thread:
	ret = (*player->get_javavm)->DetachCurrentThread(player->get_javavm);
	if (ret && !err)
		err = ERROR_COULD_NOT_DETACH_THREAD;
end:
	// TODO do something with error value
	return NULL;
}

Player * player_get_player_field(JNIEnv *env, jobject thiz) {
	jfieldID m_native_layer_field = java_get_field(env, player_class_path,
			player_mNativePlayer);
	Player *player = (Player *) (*env)->GetIntField(env, thiz,
			m_native_layer_field);
	return player;
}

void * player_fill_packet(State *state) {
	PacketData *packet_data = malloc(sizeof(PacketData));
	if (packet_data == NULL) {
		return NULL;
	}
	packet_data->packet = malloc(sizeof(AVPacket));
	if (packet_data->packet == NULL) {
		free(packet_data);
		return NULL;
	}
	return packet_data;
}

void player_free_packet(State *player, PacketData *elem) {
	free(elem->packet);
	free(elem);
}

void player_free_video_rgb_frame(State *state, VideoRGBFrameElem *elem) {
	JNIEnv *env = state->env;

	LOGI(7, "player_free_video_rgb_frame deleting global ref");
	(*env)->DeleteGlobalRef(env, elem->jbitmap);
	av_free(elem->frame);
	free(elem);
}

void *player_fill_video_rgb_frame(DecoderState *decoder_state) {
	Player *player = decoder_state->player;
	JNIEnv *env = decoder_state->env;
	jobject thiz = decoder_state->thiz;
	AVCodecContext *ctx = player->input_codec_ctxs[AVMEDIA_TYPE_VIDEO];

	VideoRGBFrameElem *elem = malloc(sizeof(VideoRGBFrameElem));
	if (elem == NULL) {
		LOGE(1,
				"player_fill_video_rgb_frame could no allocate VideoRGBFrameEelem");
		goto error;
	}

	elem->frame = avcodec_alloc_frame();
	if (elem->frame == NULL) {
		LOGE(1, "player_fill_video_rgb_frame could not create frame")
		goto free_elem;
	}

	int destWidth = ctx->width;
	int destHeight = ctx->height;

	LOGI(10, "player_fill_video_rgb_frame prepareFrame(%d, %d)", destWidth, destHeight);
	jobject jbitmap = (*env)->CallObjectMethod(env, thiz,
			player->player_prepareFrame, destWidth, destHeight);

	jthrowable exc = (*env)->ExceptionOccurred(env);
	if (exc) {
		LOGE(1, "player_fill_video_rgb_frame could not create jbitmap - exception occure");
		goto free_frame;
	}
	if (jbitmap == NULL) {
		LOGE(1, "player_fill_video_rgb_frame could not create jbitmap");
		goto free_frame;
	}

	elem->jbitmap = (*env)->NewGlobalRef(env, jbitmap);
	if (elem->jbitmap == NULL) {
		goto free_frame;
	}
	(*env)->DeleteLocalRef(env, jbitmap);

	goto end;

	(*env)->DeleteGlobalRef(env, elem->jbitmap);
	elem->jbitmap = NULL;

free_frame:
	av_freep(&elem->frame);

free_elem:
	free(elem);
	elem = NULL;

error:
end:
	return elem;
}

void player_update_current_time(State *state, int is_finished) {
	Player *player = state->player;
	jboolean jis_finished = is_finished ? JNI_TRUE : JNI_FALSE;

	(*state->env)->CallVoidMethod(state->env, state->thiz,
		player->player_onUpdateTime, player->last_updated_time,
		player->video_duration, jis_finished);
}

void player_update_time(State *state, double time) {
	int time_int = round(time);

	Player *player = state->player;
	if (player->last_updated_time == time_int) {
		return;
	}
	player->last_updated_time = time_int;

	// because video duration can be estimate
	// we have to ensure that it will not be smaller
	// than current time
	if (time_int > player->video_duration)
		player->video_duration = time_int;

	player_update_current_time(state, FALSE);
}

void player_free_streams(Player *player) {
	int i;
	for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
		if (player->input_codec_ctxs[i]) {
			avcodec_close(player->input_codec_ctxs[i]);
			player->input_codec_ctxs[i] = NULL;
		}
		player->input_streams[i] = NULL;
		player->input_frames[i] = NULL;
		player->stream_indexs[i] = -1;
	}
	player->video_index = -1;
	player->audio_index = -1;
}

uint64_t player_find_layout_from_channels(int nb_channels) {
	int i;
	for (i = 0; i < FF_ARRAY_ELEMS(channel_android_layout_map); i++)
		if (nb_channels == channel_android_layout_map[i].nb_channels)
			return channel_android_layout_map[i].layout;
	return (uint64_t) 0;
}

int player_free_frames(Player *player) {
	int i;
	for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
		if (player->input_frames[i] != NULL) {
			av_freep(&player->input_frames[i]);
		}
	}
	return 0;
}

int player_alloc_frames(Player *player) {
	int i;
	for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
		if (player->input_codec_ctxs[i]) {
			player->input_frames[i] = avcodec_alloc_frame();
			if (player->input_frames[i] == NULL) {
				return -ERROR_COULD_NOT_ALLOC_FRAME;
			}
		}
	}
	return 0;
}

int player_alloc_queues(State *state) {
	Player *player = state->player;
	int i;
	for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
		if (player->input_codec_ctxs[i]) {
			player->packets_queue[i] = queue_init_with_custom_lock(100,
				(queue_fill_func) player_fill_packet,
				(queue_free_func) player_free_packet, state, state,
				&player->mutex_queue, &player->cond_queue);
			if (player->packets_queue[i] == NULL) {
				return -ERROR_COULD_NOT_PREPARE_PACKETS_QUEUE;
			}
		}
	}
	return 0;
}

void player_free_queues(State *state) {
	Player *player = state->player;
	int i;
	for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
		if (player->packets_queue[i] != NULL) {
			queue_free(player->packets_queue[i], &player->mutex_queue, &player->cond_queue, state);
			player->packets_queue[i] = NULL;
		}
	}
}

void player_free_rgb_frames(State *state) {
	Player *player = state->player;
	if (player->rgb_video_queue != NULL) {
		LOGI(7, "player_set_data_source free_video_frames_queue");
		queue_free(player->rgb_video_queue, &player->mutex_queue, &player->cond_queue, state);
		player->rgb_video_queue = NULL;
		LOGI(7, "player_set_data_source fried_video_frames_queue");
	}
}

int player_prepare_rgb_frames(DecoderState *decoder_state, State *state) {
	Player *player = decoder_state->player;

	player->rgb_video_queue = queue_init_with_custom_lock(8,
		(queue_fill_func) player_fill_video_rgb_frame,
		(queue_free_func) player_free_video_rgb_frame, decoder_state,
		state, &player->mutex_queue, &player->cond_queue);
	if (player->rgb_video_queue == NULL) {
		return -ERROR_COULD_NOT_PREPARE_RGB_QUEUE;
	}
	return 0;
}

int player_preapre_sws_context(Player *player) {
	AVCodecContext *ctx = player->input_codec_ctxs[AVMEDIA_TYPE_VIDEO];
	int destWidth = ctx->width;
	int destHeight = ctx->height;

	player->sws_context = sws_getContext(ctx->width, ctx->height, ctx->pix_fmt,
			destWidth, destHeight, player->out_format, SWS_BICUBIC, NULL, NULL,
			NULL);
	if (player->sws_context == NULL) {
		LOGE(1, "could not initialize conversion context from: %d"
				", to :%d\n", ctx->pix_fmt, player->out_format);
		return -ERROR_COULD_NOT_GET_SWS_CONTEXT;
	}
	return 0;
}

void player_free_sws_context(Player *player) {
	if (player->sws_context != NULL) {
		LOGI(7, "player_set_data_source free_sws_context");
		sws_freeContext(player->sws_context);
		player->sws_context = NULL;
	}
}

void player_free_audio_track(Player *player, State *state) {
	if (player->swr_context != NULL) {
		swr_free(&player->swr_context);
		player->swr_context = NULL;
	}

	if (player->audio_track != NULL) {
		LOGI(7, "player_set_data_source free_audio_track_ref");
		(*state->env)->DeleteGlobalRef(state->env, player->audio_track);
		player->audio_track = NULL;
	}
	if (player->audio_index >= 0) {
		AVCodecContext **ctx = &player->input_codec_ctxs[AVMEDIA_TYPE_AUDIO];
		if (*ctx != NULL) {
			LOGI(7, "player_set_data_sourceclose_audio_codec");
			avcodec_close(*ctx);
			*ctx = NULL;
		}
	}
}

int player_create_audio_track(Player *player, State *state) {
	AVCodecContext *ctx = player->input_codec_ctxs[AVMEDIA_TYPE_AUDIO];
	int sample_rate = ctx->sample_rate;
	int channels = ctx->channels;

	jobject audio_track = (*state->env)->CallObjectMethod(state->env,
		state->thiz, player->player_prepareAudioTrack, sample_rate, channels);

	jthrowable exc = (*state->env)->ExceptionOccurred(state->env);
	if (exc) {
		return -ERROR_NOT_CREATED_AUDIO_TRACK;
	}
	if (audio_track == NULL) {
		return -ERROR_NOT_CREATED_AUDIO_TRACK;
	}

	player->audio_track = (*state->env)->NewGlobalRef(state->env, audio_track);
	(*state->env)->DeleteLocalRef(state->env, audio_track);
	if (player->audio_track == NULL) {
		return -ERROR_NOT_CREATED_AUDIO_TRACK_GLOBAL_REFERENCE;
	}

	player->audio_track_channel_count = (*state->env)->CallIntMethod(state->env,
		player->audio_track, player->audio_track_getChannelCount);
	int audio_track_sample_rate = (*state->env)->CallIntMethod(state->env,
		player->audio_track, player->audio_track_getSampleRate);
	player->audio_track_format = AV_SAMPLE_FMT_S16;

	int64_t audio_track_layout = player_find_layout_from_channels(
		player->audio_track_channel_count);

	int64_t dec_channel_layout = (ctx->channel_layout &&
		ctx->channels == av_get_channel_layout_nb_channels(ctx->channel_layout)) ?
		ctx->channel_layout : av_get_default_channel_layout(ctx->channels);

	player->swr_context = NULL;
	if (ctx->sample_fmt != player->audio_track_format
		|| dec_channel_layout != audio_track_layout
		|| ctx->sample_rate != audio_track_sample_rate) {
		LOGI(3,
				"player_set_data_sourcd preparing conversion of %d Hz %s %d channels to %d Hz %s %d channels",
				ctx->sample_rate, av_get_sample_fmt_name(ctx->sample_fmt), ctx->channels,
				audio_track_sample_rate, av_get_sample_fmt_name(player->audio_track_format),
				player->audio_track_channel_count);
		player->swr_context = (struct SwrContext *) swr_alloc_set_opts(NULL,
			audio_track_layout, player->audio_track_format,
			audio_track_sample_rate, dec_channel_layout, ctx->sample_fmt,
			ctx->sample_rate, 0, NULL);

		if (!player->swr_context || swr_init(player->swr_context) < 0) {
			LOGE(1,
					"Cannot create sample rate converter for conversion of %d Hz %s %d "
					"channels to %d Hz %s %d channels!", ctx->sample_rate,
					av_get_sample_fmt_name(ctx->sample_fmt), ctx->channels,
					audio_track_sample_rate, av_get_sample_fmt_name(player->audio_track_format),
					player->audio_track_channel_count);
			return -ERROR_COULD_NOT_INIT_SWR_CONTEXT;
		}
	}
	return 0;
}

void player_get_video_duration(Player *player) {
	player->last_updated_time = -1;
	player->video_duration = 0;
	int i;

	for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
		AVStream *stream = player->input_streams[i];
		if (stream && stream->duration > 0) {
			player->video_duration = round(stream->duration * av_q2d(stream->time_base));
			LOGI(3,
					"player_set_data_source stream[%d] duration: %lld", i, stream->duration);
			return;
		}
	}
	if (player->format_ctx->duration != 0) {
		player->video_duration = round(
				player->format_ctx->duration * av_q2d(AV_TIME_BASE_Q));
		LOGI(3,
				"player_set_data_source video duration: %lld", player->format_ctx->duration)
		return;
	}

	for (i = 0; i < player->format_ctx->nb_streams; i++) {
		AVStream *stream = player->format_ctx->streams[i];
		if (stream->duration > 0) {
			player->video_duration = round(
					stream->duration * av_q2d(stream->time_base));
			LOGI(3,
					"player_set_data_source stream[%d] duration: %lld", i, stream->duration);
			return;
		}
	}
}

int player_create_decoding_threads(Player *player) {
	pthread_attr_t attr;
	int ret;
	int i;
	int err = 0;

	ret = pthread_attr_init(&attr);
	if (ret) {
		err = -ERROR_COULD_NOT_INIT_PTHREAD_ATTR;
		goto end;
	}
	for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
		if (player->input_codec_ctxs[i]) {
			DecoderData *decoder_data = malloc(sizeof(DecoderData));
			*decoder_data = (DecoderData) {player, (enum AVMediaType)i};
			ret = pthread_create(&player->decode_threads[i], &attr, player_decode,
				decoder_data);
			if (ret) {
				err = -ERROR_COULD_NOT_CREATE_PTHREAD;
				goto end;
			}
			player->decode_threads_created[i] = TRUE;
		}
	}

	ret = pthread_create(&player->read_stream_thread, &attr,
			player_read_stream, player);
	if (ret) {
		err = -ERROR_COULD_NOT_CREATE_PTHREAD;
		goto end;
	}
	player->read_stream_thread_created = TRUE;

end:
	ret = pthread_attr_destroy(&attr);
	if (ret) {
		if (!err) {
			err = ERROR_COULD_NOT_DESTROY_PTHREAD_ATTR;
		}
	}
	return err;
}

int player_free_decoding_threads(Player *player) {
	int i, ret, err = 0;

	if (player->read_stream_thread_created) {
		LOGI(3, "pthread_join: read_stream_thread begin");
		ret = pthread_join(player->read_stream_thread, NULL);
		LOGI(3, "pthread_join: read_stream_thread end");
		player->read_stream_thread_created = FALSE;
		if (ret) {
			err = ERROR_COULD_NOT_JOIN_PTHREAD;
		}
	}

	for (i = 0; i < AVMEDIA_TYPE_NB; ++i) {
		if (player->decode_threads_created[i]) {
			LOGI(3, "pthread_join: decode_threads[%d] begin", i);
			ret = pthread_join(player->decode_threads[i], NULL);
			LOGI(3, "pthread_join: decode_threads[%d] end", i);
			player->decode_threads_created[i] = FALSE;
			if (ret) {
				err = ERROR_COULD_NOT_JOIN_PTHREAD;
			}
		}
	}
	return err;
}

void player_free_input(Player *player) {
	if (player->input_inited) {
		LOGI(7, "player_set_data_source close_file");
		avformat_close_input(&player->format_ctx);
		player->input_inited = FALSE;
	}
}

/* we interrupt if |avformat_open_input()| has elapsed 7 second */
static int decoder_interrupt_cb(void *ctx) {
	Player *player= ctx;
	return (player->stop) || (player->open_time && (av_gettime() - player->open_time) > 7LL*AV_TIME_BASE) ;
}

int player_open_input(Player *player, const char *file_path, AVDictionary *dictionary) {
	AVFormatContext *ic = NULL;
	int ret;

	ic = avformat_alloc_context();
	ic->interrupt_callback.callback = decoder_interrupt_cb;
	ic->interrupt_callback.opaque   = player;

	player->open_time = av_gettime();
	if ((ret = avformat_open_input(&ic, file_path, NULL, &dictionary)) < 0) {
		char errbuf[128];
		const char *errbuf_ptr = errbuf;

		avformat_free_context(ic);
		if (av_strerror(ret, errbuf, sizeof(errbuf)) < 0)
			errbuf_ptr = strerror(AVUNERROR(ret));

		LOGE(1,
				"player_set_data_source Could not open video file: %s (%d: %s)\n", file_path, ret, errbuf_ptr);
		return -ERROR_COULD_NOT_OPEN_VIDEO_FILE;
	}
	player->format_ctx = ic;
	player->open_time = 0;
	player->input_inited = TRUE;
	update_external_clock_pts(player, av_gettime()/(double)AV_NOPTS_VALUE);
	update_external_clock_speed(player, 1.0);
	player->video_current_pts_drift = -av_gettime() / 1000000.0;

	return ERROR_NO_ERROR;
}

int player_find_stream_info(Player *player) {
	if (avformat_find_stream_info(player->format_ctx, NULL) < 0) {
		LOGE(1, "Could not open stream\n");
		return -ERROR_COULD_NOT_OPEN_STREAM;
	}
	return ERROR_NO_ERROR;
}

static void player_signal_stop(Player *player) {
	pthread_mutex_lock(&player->mutex_queue);
	player->stop = TRUE;
	pthread_cond_broadcast(&player->cond_queue);
	pthread_mutex_unlock(&player->mutex_queue);
}

void player_play_prepare(Player *player) {
	LOGI(3, "player_set_data_source 16");
	pthread_mutex_lock(&player->mutex_queue);
	player->stop = FALSE;
	player->seek_position = DO_NOT_SEEK;
	player_assign_to_no_boolean_array(player, player->flush_streams, FALSE);
	player_assign_to_no_boolean_array(player, player->stop_streams, FALSE);

	pthread_cond_broadcast(&player->cond_queue);
	pthread_mutex_unlock(&player->mutex_queue);
}

void player_stop(State * state) {
	Player *player = state->player;

	LOGI(3, "player_stop try to stop...");
	if (!player->playing)
		return;

	pthread_mutex_lock(&state->player->mutex_operation);

	if (!player->playing) {
		pthread_mutex_unlock(&player->mutex_operation);
		return;
	}
	player->playing = FALSE;

	LOGI(3, "player_stop stopping...");
	player_signal_stop(player);
	player_free_decoding_threads(player);
	player_free_audio_track(player, state);
	player_free_sws_context(player);
	player_free_rgb_frames(state);
	player_free_queues(state);
	player_free_frames(player);
	player_free_streams(player);
	player_free_input(player);
	LOGI(3, "player_stop stopped...");

	pthread_mutex_unlock(&player->mutex_operation);
}

static int stream_component_open(Player *player, int stream_index) {
	AVFormatContext *ic = player->format_ctx;
	AVStream *st;
	AVCodecContext *avctx;
	AVCodec *codec;
	int channels = -1, sample_rate = -1, frame_size = -1;

	if (stream_index < 0 || stream_index > ic->nb_streams)
		return -1;

	st = ic->streams[stream_index];
	avctx = st->codec;
	codec = avcodec_find_decoder(avctx->codec_id);
	if (!codec) {
		LOGI(1, "No codec could be found with id %d", avctx->codec_id);
		return -1;
	}

	st->discard = AVDISCARD_DEFAULT;
	switch(avctx->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
		player->audio_index = stream_index;
		player->input_streams[AVMEDIA_TYPE_AUDIO] = st;
		player->input_codec_ctxs[AVMEDIA_TYPE_AUDIO] = avctx;
		player->stream_indexs[AVMEDIA_TYPE_AUDIO] = stream_index;
		channels    = avctx->channels;
		sample_rate = avctx->sample_rate;
		frame_size  = avctx->frame_size;
		break;
	case AVMEDIA_TYPE_VIDEO:
		player->video_index = stream_index;
		player->input_streams[AVMEDIA_TYPE_VIDEO] = st;
		player->input_codec_ctxs[AVMEDIA_TYPE_VIDEO] = avctx;
		player->stream_indexs[AVMEDIA_TYPE_VIDEO] = stream_index;
		break;
	default:
		break;
	}

	//TODO: setup |avctx| options
	if (avctx->lowres)
		avctx->flags |= CODEC_FLAG_EMU_EDGE;
	if (codec->capabilities & CODEC_CAP_DR1)
		avctx->flags |= CODEC_FLAG_EMU_EDGE;
	if (avcodec_open2(avctx, codec, NULL) < 0)
		return -1;
	if (avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
		/* |avcodec_open2()| could break audio codec settings */
		if (avctx->channels <= 0)
			avctx->channels = channels;
		if (avctx->sample_rate <= 0)
			avctx->sample_rate = sample_rate;
		if (avctx->frame_size <= 0)
			avctx->frame_size = frame_size;
	}
	return 0;
}

int player_set_data_source(State *state, const char *file_path,
		AVDictionary *dictionary, int video_index, int audio_index,
		int subtitle_index) {
	Player *player = state->player;
	AVFormatContext *ic;
	int st_index[AVMEDIA_TYPE_NB];
	int i, err = ERROR_NO_ERROR;

	pthread_mutex_lock(&player->mutex_operation);

	if (player->playing) {
	    pthread_mutex_unlock(&player->mutex_operation);
	    return ERROR_NOT_STOP_LAST_INSTANCE;
	}

	player->out_format = AV_PIX_FMT_RGB565;
	player->pause = TRUE;
	player->audio_pause_time = player->audio_resume_time = av_gettime();
	memset(player->stream_indexs, -1, sizeof(player->stream_indexs));
	memset(player->input_codec_ctxs, 0, sizeof(player->input_codec_ctxs));
	memset(player->input_frames, 0, sizeof(player->input_frames));
	memset(player->packets_queue, 0, sizeof(player->packets_queue));
	memset(player->decode_threads, 0, sizeof(player->decode_threads));
	memset(player->decode_threads_created, 0, sizeof(player->decode_threads_created));

	player->stream_indexs[AVMEDIA_TYPE_VIDEO   ] = video_index;
	player->stream_indexs[AVMEDIA_TYPE_AUDIO   ] = audio_index;
	player->stream_indexs[AVMEDIA_TYPE_SUBTITLE] = subtitle_index;
	memset(st_index, -1, sizeof(st_index));

	if ((err = player_open_input(player, file_path, dictionary)) < 0)
		goto error;

	if ((err = player_find_stream_info(player)) < 0)
		goto error;

	ic = player->format_ctx;
	for (i=0; i<ic->nb_streams; i++) {
		ic->streams[i]->discard = AVDISCARD_ALL;
	}
	st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
		player->stream_indexs[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
	st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
		player->stream_indexs[AVMEDIA_TYPE_AUDIO], -1, NULL, 0);

	if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
		err = stream_component_open(player, st_index[AVMEDIA_TYPE_AUDIO]);
		if (err < 0)
			goto error;
		err = player_create_audio_track(player, state);
		if (err < 0)
			goto error;
	}
	if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
		err = stream_component_open(player, st_index[AVMEDIA_TYPE_VIDEO]);
		if (err < 0)
			goto error;
		DecoderState video_decoder_state = { player->video_index, AVMEDIA_TYPE_VIDEO, player, state->env, state->thiz };
		err = player_prepare_rgb_frames(&video_decoder_state, state);
		if (err < 0)
			goto error;
		if ((err = player_preapre_sws_context(player)) < 0)
			goto error;
	}

	if ((err = player_alloc_frames(player)) < 0)
		goto error;

	if ((err = player_alloc_queues(state)) < 0)
		goto error;

	player_get_video_duration(player);
	player_update_time(state, 0.0);

	player_play_prepare(player);

	if ((err = player_create_decoding_threads(player)) < 0) {
		goto error;
	}

	player->playing = TRUE;
	LOGI(3, "player_set_data_source success");
	pthread_mutex_unlock(&player->mutex_operation);
	return err;

error:
	LOGI(3, "player_set_data_source error");

	player_signal_stop(player);
	player_free_decoding_threads(player);
	player_free_audio_track(player, state);
	player_free_sws_context(player);
	player_free_rgb_frames(state);
	player_free_queues(state);
	player_free_frames(player);
	player_free_streams(player);
	player_free_input(player);
	pthread_mutex_unlock(&player->mutex_operation);
	return err;
}

void jni_player_seek(JNIEnv *env, jobject thiz, jint position) {
	Player *player = player_get_player_field(env, thiz);

	pthread_mutex_lock(&player->mutex_operation);
	if (!player->playing) {
		LOGI(1, "jni_player_seek could not seek while not playing");
		throw_exception(env, not_playing_exception_class_path,
				"Could not pause while not playing");
		goto end;
	}
	pthread_mutex_lock(&player->mutex_queue);
	player->seek_position = position;
	pthread_cond_broadcast(&player->cond_queue);

	while (player->seek_position != DO_NOT_SEEK)
		pthread_cond_wait(&player->cond_queue, &player->mutex_queue);
	pthread_mutex_unlock(&player->mutex_queue);
end:
	pthread_mutex_unlock(&player->mutex_operation);
}

void jni_player_pause(JNIEnv *env, jobject thiz) {
	Player *player = player_get_player_field(env, thiz);

	pthread_mutex_lock(&player->mutex_operation);

	if (!player->playing) {
		LOGI(1, "jni_player_pause could not pause while not playing");
		throw_exception(env, not_playing_exception_class_path,
				"Could not pause while not playing");
		goto end;
	}

	pthread_mutex_lock(&player->mutex_queue);
	if (player->pause)
		goto do_nothing;
	LOGI(3, "jni_player_pause Pausing");
	update_external_clock_pts(player, get_external_clock(player));
	player->pause = TRUE;

	if (player->audio_track) {
		(*env)->CallVoidMethod(env, player->audio_track, player->audio_track_pause);
	}

	player->audio_pause_time = av_gettime();

	pthread_cond_broadcast(&player->cond_queue);

do_nothing:
	pthread_mutex_unlock(&player->mutex_queue);
end:
	pthread_mutex_unlock(&player->mutex_operation);
}

void jni_player_resume(JNIEnv *env, jobject thiz) {
	Player *player = player_get_player_field(env, thiz);
	pthread_mutex_lock(&player->mutex_operation);

	if (!player->playing) {
		LOGI(1, "jni_player_resume could not pause while not playing");
		throw_exception(env, not_playing_exception_class_path,
				"Could not resume while not playing");
		goto end;
	}

	pthread_mutex_lock(&player->mutex_queue);
	if (!player->pause)
		goto do_nothing;
	player->pause = FALSE;

	if (player->audio_track) {
		(*env)->CallVoidMethod(env, player->audio_track, player->audio_track_play);
	}

	player->audio_resume_time = av_gettime();
	if (player->audio_write_time < player->audio_pause_time) {
		player->audio_write_time = player->audio_resume_time;
	} else if (player->audio_write_time < player->audio_resume_time) {
		player->audio_write_time += player->audio_resume_time - player->audio_pause_time;
	}
	player->video_current_pts_drift = player->video_current_pts - av_gettime() / 1000000.0;
	update_external_clock_pts(player, get_external_clock(player));

	pthread_cond_broadcast(&player->cond_queue);

do_nothing:
	pthread_mutex_unlock(&player->mutex_queue);
end:
	pthread_mutex_unlock(&player->mutex_operation);
}

void jni_player_read_dictionary(JNIEnv *env, AVDictionary **dictionary, jobject jdictionary) {
	jclass map_class = (*env)->FindClass(env, map_class_path);
	jclass set_class = (*env)->FindClass(env, set_class_path);
	jclass iterator_class = (*env)->FindClass(env, iterator_class_path);

	jmethodID map_key_set_method = java_get_method(env, map_class, map_key_set);
	jmethodID map_get_method = java_get_method(env, map_class, map_get);

	jmethodID set_iterator_method = java_get_method(env, set_class,
			set_iterator);

	jmethodID iterator_next_method = java_get_method(env, iterator_class,
			iterator_next);
	jmethodID iterator_has_next_method = java_get_method(env, iterator_class,
			iterator_has_next);

	jobject jkey_set = (*env)->CallObjectMethod(env, jdictionary,
			map_key_set_method);
	jobject jiterator = (*env)->CallObjectMethod(env, jkey_set,
			set_iterator_method);

	while ((*env)->CallBooleanMethod(env, jiterator, iterator_has_next_method)) {
		jobject jkey = (*env)->CallObjectMethod(env, jiterator,
			iterator_next_method);
		jobject jvalue = (*env)->CallObjectMethod(env, jdictionary,
			map_get_method, jkey);

		const char *key = (*env)->GetStringUTFChars(env, jkey, NULL);
		const char *value = (*env)->GetStringUTFChars(env, jvalue, NULL);

		if (av_dict_set(dictionary, key, value, 0) < 0) {
			LOGE(2, "player_set_data_source: could not set key");
		}

		(*env)->ReleaseStringUTFChars(env, jkey, key);
		(*env)->ReleaseStringUTFChars(env, jvalue, value);
		(*env)->DeleteLocalRef(env, jkey);
		(*env)->DeleteLocalRef(env, jvalue);
	}

	(*env)->DeleteLocalRef(env, jiterator);
	(*env)->DeleteLocalRef(env, jkey_set);

	(*env)->DeleteLocalRef(env, map_class);
	(*env)->DeleteLocalRef(env, set_class);
	(*env)->DeleteLocalRef(env, iterator_class);
}

int jni_player_set_data_source(JNIEnv *env, jobject thiz, jstring string,
		jobject dictionary, int video_index, int audio_index,
		int subtitle_index) {
	AVDictionary *dict = NULL;
	if (dictionary != NULL) {
		jni_player_read_dictionary(env, &dict, dictionary);
		(*env)->DeleteLocalRef(env, dictionary);
	}

	const char *file_path = (*env)->GetStringUTFChars(env, string, NULL);
	Player *player = player_get_player_field(env, thiz);
	State state = { player, env, thiz };

	int ret = player_set_data_source(&state, file_path, dict, video_index,
		audio_index, subtitle_index);

	(*env)->ReleaseStringUTFChars(env, string, file_path);
	return ret;
}

void jni_player_dealloc(JNIEnv *env, jobject thiz) {
	LOGI(1, "jni_player_dealloc");
	Player *player = player_get_player_field(env, thiz);

	(*env)->DeleteGlobalRef(env, player->audio_track_class);
	free(player);
}

int jni_player_init(JNIEnv *env, jobject thiz) {
#ifdef PROFILER
#warning "Profiler enabled"
	setenv("CPUPROFILE_FREQUENCY", "1000", 1);
	monstartup("libffmpeg.so");
#endif

	LOGI(1, "jni_player_init");
	Player *player = malloc(sizeof(Player));
	memset(player, 0, sizeof(Player));
	player->audio_index = -1;
	player->video_index = -1;
	player->rendering = FALSE;

	int err = ERROR_NO_ERROR;

	int ret = (*env)->GetJavaVM(env, &player->get_javavm);
	if (ret) {
		err = ERROR_COULD_NOT_GET_JAVA_VM;
		goto free_player;
	}

	{
		jclass player_class = (*env)->FindClass(env, player_class_path);

		if (player_class == NULL) {
			err = ERROR_NOT_FOUND_PLAYER_CLASS;
			goto free_player;
		}

		jfieldID player_m_native_player_field = java_get_field(env,
				player_class_path, player_mNativePlayer);
		if (player_m_native_player_field == NULL) {
			err = ERROR_NOT_FOUND_M_NATIVE_PLAYER_FIELD;
			goto free_player;
		}

		(*env)->SetIntField(env, thiz, player_m_native_player_field,
				(jint) player);

		player->player_prepareFrame = java_get_method(env, player_class,
				player_prepareFrame);
		if (player->player_prepareFrame == NULL) {
			err = ERROR_NOT_FOUND_PREPARE_FRAME_METHOD;
			goto free_player;
		}

		player->player_onUpdateTime = java_get_method(env,
				player_class, player_onUpdateTime);
		if (player->player_onUpdateTime == NULL) {
			err = ERROR_NOT_FOUND_ON_UPDATE_TIME_METHOD;
			goto free_player;
		}

		player->player_prepareAudioTrack = java_get_method(env,
				player_class, player_prepareAudioTrack);
		if (player->player_prepareAudioTrack == NULL) {
			err = ERROR_NOT_FOUND_PREPARE_AUDIO_TRACK_METHOD;
			goto free_player;
		}

		(*env)->DeleteLocalRef(env, player_class);
	}

	{
		jclass audio_track_class = (*env)->FindClass(env,
				android_track_class_path);
		if (audio_track_class == NULL) {
			err = ERROR_NOT_FOUND_AUDIO_TRACK_CLASS;
			goto free_player;
		}

		player->audio_track_class = (*env)->NewGlobalRef(env,
				audio_track_class);
		if (player->audio_track_class == NULL) {
			err = ERROR_COULD_NOT_CREATE_GLOBAL_REF_FOR_AUDIO_TRACK_CLASS;
			(*env)->DeleteLocalRef(env, audio_track_class);
			goto free_player;
		}
		(*env)->DeleteLocalRef(env, audio_track_class);
	}

	player->audio_track_write = java_get_method(env, player->audio_track_class, audio_track_write);
	if (player->audio_track_write == NULL) {
		err = ERROR_NOT_FOUND_WRITE_METHOD;
		goto delete_audio_track_global_ref;
	}

	player->audio_track_play = java_get_method(env, player->audio_track_class, audio_track_play);
	if (player->audio_track_play == NULL) {
		err = ERROR_NOT_FOUND_PLAY_METHOD;
		goto delete_audio_track_global_ref;
	}

	player->audio_track_pause = java_get_method(env, player->audio_track_class, audio_track_pause);
	if (player->audio_track_pause == NULL) {
		err = ERROR_NOT_FOUND_PAUSE_METHOD;
		goto delete_audio_track_global_ref;
	}

	player->audio_track_flush = java_get_method(env, player->audio_track_class, audio_track_flush);
	if (player->audio_track_flush == NULL) {
		err = ERROR_NOT_FOUND_FLUSH_METHOD;
		goto delete_audio_track_global_ref;
	}

	player->audio_track_stop = java_get_method(env, player->audio_track_class, audio_track_stop);
	if (player->audio_track_stop == NULL) {
		err = ERROR_NOT_FOUND_STOP_METHOD;
		goto delete_audio_track_global_ref;
	}

	player->audio_track_getChannelCount = java_get_method(env, player->audio_track_class,
		audio_track_getChannelCount);
	if (player->audio_track_getChannelCount == NULL) {
		err = ERROR_NOT_FOUND_GET_CHANNEL_COUNT_METHOD;
		goto delete_audio_track_global_ref;
	}

	player->audio_track_getSampleRate = java_get_method(env,
		player->audio_track_class, audio_track_getSampleRate);
	if (player->audio_track_getSampleRate == NULL) {
		err = ERROR_NOT_FOUND_GET_SAMPLE_RATE_METHOD;
		goto delete_audio_track_global_ref;
	}

	pthread_mutex_init(&player->mutex_operation, NULL);
	pthread_mutex_init(&player->mutex_queue, NULL);
	pthread_cond_init(&player->cond_queue, NULL);

	player->playing = FALSE;
	player->pause = FALSE;
	player->stop = FALSE;
	player->flush_video_play = FALSE;

	av_log_set_level(AV_LOG_WARNING);
	avformat_network_init();
	av_register_all();
#ifdef MODULE_JNI_PROTOCOL
	register_jni_protocol(player->get_javavm);
#endif
#ifdef MODULE_ENCRYPT
	register_aes_protocol();
#endif

	goto end;

delete_audio_track_global_ref:
	(*env)->DeleteGlobalRef(env, player->audio_track_class);
free_player:
	free(player);
end:
	return err;
}

QueueCheckFuncRet player_render_frame_check(Queue *queue, Player *player, int *check_ret_data) {
	if (player->interrupt_renderer) {
		*check_ret_data = RENDER_CHECK_MSG_INTERRUPT;
		LOGI(6, "player_render_frame_check: interrupt_renderer")
		return QUEUE_CHECK_FUNC_RET_SKIP;
	}
	if (player->flush_video_play) {
		LOGI(6, "player_render_frame_check: flush_video_play");
		*check_ret_data = RENDER_CHECK_MSG_FLUSH;
		return QUEUE_CHECK_FUNC_RET_SKIP;
	}
	if (player->pause) {
		LOGI(6, "player_render_frame_check: pause")

		return QUEUE_CHECK_FUNC_RET_WAIT;
	}
	if (player->stop) {
		LOGI(6, "player_render_frame_check: stop")

		return QUEUE_CHECK_FUNC_RET_WAIT;
	}

	LOGI(9, "player_render_frame_check: test")
	return QUEUE_CHECK_FUNC_RET_TEST;
}

void jni_player_render_frame_start(JNIEnv *env, jobject thiz) {
	Player *player = player_get_player_field(env, thiz);
	pthread_mutex_lock(&player->mutex_queue);
	assert(!player->rendering);
	player->rendering = TRUE;
	player->interrupt_renderer = FALSE;
	pthread_cond_broadcast(&player->cond_queue);
	pthread_mutex_unlock(&player->mutex_queue);
}

void jni_player_render_frame_stop(JNIEnv *env, jobject thiz) {
	Player *player = player_get_player_field(env, thiz);
	pthread_mutex_lock(&player->mutex_queue);
	assert(player->rendering);
	player->rendering = FALSE;
	player->interrupt_renderer = TRUE;
	pthread_cond_broadcast(&player->cond_queue);
	pthread_mutex_unlock(&player->mutex_queue);
}

jobject jni_player_render_frame(JNIEnv *env, jobject thiz) {
	Player *player = player_get_player_field(env, thiz);
	State state = { player, env, thiz };
	int interrupt_ret;
	VideoRGBFrameElem *elem;

	LOGI(7, "jni_player_render_frame render wait...");
	pthread_mutex_lock(&player->mutex_queue);

pop:
	LOGI(4, "jni_player_render_frame reading from queue");
	elem = queue_pop_start_impl(&player->rgb_video_queue,
			&player->mutex_queue, &player->cond_queue,
			(QueueCheckFunc) player_render_frame_check, player,
			&interrupt_ret);
	for (;;) {
		int skip = FALSE;
		if (elem == NULL) {
			skip = TRUE;
		} else {
			if (elem->end_of_stream) {
				LOGI(4, "jni_player_render_frame end of stream");
				player_update_current_time(&state, TRUE);
				queue_pop_finish_impl(player->rgb_video_queue,
						&player->mutex_queue, &player->cond_queue);
				goto pop;
			}
			QueueCheckFuncRet ret;
test:
			ret = player_render_frame_check(player->rgb_video_queue,
					player, &interrupt_ret);
			switch (ret) {
			case QUEUE_CHECK_FUNC_RET_WAIT:
				LOGI(1, "jni_player_render_frame queue wait");
				pthread_cond_wait(&player->cond_queue, &player->mutex_queue);
				goto test;
			case QUEUE_CHECK_FUNC_RET_SKIP:
				skip = TRUE;
				LOGI(1, "jni_player_render_frame queue skip");
				queue_pop_finish_impl(player->rgb_video_queue, &player->mutex_queue, &player->cond_queue);
				break;
			case QUEUE_CHECK_FUNC_RET_TEST:
				break;
			default:
				assert(FALSE);
				break;
			}
		}
		if (skip) {
			if (interrupt_ret == RENDER_CHECK_MSG_INTERRUPT) {
				LOGI(2, "jni_player_render_frame interrupted");
				pthread_mutex_unlock(&player->mutex_queue);
				throw_interrupted_exception(env, "Render frame was interrupted by user");
				return NULL;
			} else if (interrupt_ret == RENDER_CHECK_MSG_FLUSH) {
				LOGI(2, "jni_player_render_frame flush");
				VideoRGBFrameElem *elem;
				while ((elem = queue_pop_start_impl_non_block(
						player->rgb_video_queue)) != NULL) {
					queue_pop_finish_impl(player->rgb_video_queue, &player->mutex_queue, &player->cond_queue);
				}
				LOGI(2, "jni_player_render_frame flushed");
				player->flush_video_play = FALSE;
				pthread_cond_broadcast(&player->cond_queue);
				goto pop;
			} else {
				assert(FALSE);
			}
		}

		int64_t current_time = av_gettime();
		int64_t time_diff = current_time - player->audio_write_time;
		double pts_time_diff_d = elem->time - player->audio_clock;
		int64_t sleep_time = (int64_t) (pts_time_diff_d * 1000.0)
				- (int64_t) (time_diff / 1000L);
		LOGI(9,
				"jni_player_render_frame current_time: "
				"%lld, write_time: %lld, time_diff: %lld, "
				"elem->time: %f, player->audio_clock: %f "
				"sleep_time: %lld",
				current_time, player->audio_write_time, time_diff,
				elem->time, player->audio_clock, sleep_time);
		if (!player->audio_track) {
			double video_clock = get_video_clock(player);
			sleep_time = (elem->time - video_clock) * 1000LL;
			LOGI(9,"jni_player_render_frame: video only mode:video_clock %f sleep_time %lld",
					video_clock, sleep_time);
		}

		if (sleep_time <= MIN_SLEEP_TIME_MS) {
			break;
		}

		if (sleep_time > 1000) {
			sleep_time = 1000;
		}

		int ret = pthread_cond_timeout_np(&player->cond_queue, &player->mutex_queue, sleep_time);
		if (ret == ETIMEDOUT) {
			LOGI(9, "jni_player_render_frame timeout");
			break;
		}
		LOGI(9, "jni_player_render_frame condition occurs");
	}
	player_update_time(&state, elem->time);
	update_video_pts(player,elem->time);
	pthread_mutex_unlock(&player->mutex_queue);

	LOGI(7, "jni_player_render_frame rendering...");

	return elem->jbitmap;
}

void jni_player_release_frame(JNIEnv *env, jobject thiz) {
	Player *player = player_get_player_field(env, thiz);
	queue_pop_finish(player->rgb_video_queue, &player->mutex_queue, &player->cond_queue);
	LOGI(7, "jni_player_release_frame rendered");
}

void jni_player_stop(JNIEnv *env, jobject thiz) {
#ifdef PROFILER
	moncleanup();
#endif

	Player *player = player_get_player_field(env, thiz);
	State state;

	state.player = player;
	state.env = env;
	state.thiz = thiz;

	player_stop(&state);
}

int jni_player_get_video_duration(JNIEnv *env, jobject thiz) {
	Player *player = player_get_player_field(env, thiz);
	return player->video_duration;
}
