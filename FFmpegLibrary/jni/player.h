/*
 * player.h
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

#ifndef H_PLAYER
#define H_PLAYER

#include <libavutil/audioconvert.h>

static JavaMethod empty_constructor = {"<init>", "()V"};

// InterruptedException
static char *interrupted_exception_class_path_name = "java/lang/InterruptedException";

// RuntimeException
static char *runtime_exception_class_path_name = "java/lang/RuntimeException";

// NotPlayingException
static char *not_playing_exception_class_path_name = "net/uplayer/ffmpeg/NotPlayingException";

// Object
static char *object_class_path_name = "java/lang/Object";

// HashMap
static char *hash_map_class_path_name = "java/util/HashMap";
static char *map_class_path_name = "java/util/Map";
static JavaMethod map_key_set = {"keySet", "()Ljava/util/Set;"};
static JavaMethod map_get = {"get", "(Ljava/lang/Object;)Ljava/lang/Object;"};
static JavaMethod map_put = {"put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"};

// FFmpegStreamInfo.CodeType
enum CodecType {
	CODEC_TYPE_UNKNOWN = 0,
	CODEC_TYPE_AUDIO = 1,
	CODEC_TYPE_VIDEO = 2,
	CODEC_TYPE_SUBTITLE = 3,
	CODEC_TYPE_ATTACHMENT = 4,
	CODEC_TYPE_NB = 5,
	CODEC_TYPE_DATA = 6
};

enum Msgs {
	MSG_NONE = 0, MSG_STOP = 1
};

enum PlayerErrors {
	ERROR_NO_ERROR = 0,

	// Java errors
	ERROR_NOT_FOUND_PLAYER_CLASS,
	ERROR_NOT_FOUND_PREPARE_FRAME_METHOD,
	ERROR_NOT_FOUND_ON_UPDATE_TIME_METHOD,
	ERROR_NOT_FOUND_PREPARE_AUDIO_TRACK_METHOD,
	ERROR_NOT_FOUND_SET_STREAM_INFO_METHOD,
	ERROR_NOT_FOUND_M_NATIVE_PLAYER_FIELD,
	ERROR_COULD_NOT_GET_JAVA_VM,
	ERROR_COULD_NOT_DETACH_THREAD,
	ERROR_COULD_NOT_ATTACH_THREAD,
	ERROR_COULD_NOT_CREATE_GLOBAL_REF_FOR_AUDIO_TRACK_CLASS,

	// AudioTrack
	ERROR_NOT_FOUND_AUDIO_TRACK_CLASS,
	ERROR_NOT_FOUND_WRITE_METHOD,
	ERROR_NOT_FOUND_PLAY_METHOD,
	ERROR_NOT_FOUND_PAUSE_METHOD,
	ERROR_NOT_FOUND_STOP_METHOD,
	ERROR_NOT_FOUND_GET_CHANNEL_COUNT_METHOD,
	ERROR_NOT_FOUND_FLUSH_METHOD,
	ERROR_NOT_FOUND_GET_SAMPLE_RATE_METHOD,

	ERROR_COULD_NOT_CREATE_AVCONTEXT,
	ERROR_COULD_NOT_OPEN_VIDEO_FILE,
	ERROR_COULD_NOT_OPEN_STREAM,
	ERROR_COULD_NOT_OPEN_VIDEO_STREAM,
	ERROR_COULD_NOT_FIND_VIDEO_CODEC,
	ERROR_COULD_NOT_OPEN_VIDEO_CODEC,
	ERROR_COULD_NOT_ALLOC_FRAME,

	ERROR_NOT_CREATED_BITMAP,
	ERROR_COULD_NOT_GET_SWS_CONTEXT,
	ERROR_COULD_NOT_PREPARE_PACKETS_QUEUE,
	ERROR_COULD_NOT_FIND_AUDIO_STREAM,
	ERROR_COULD_NOT_FIND_AUDIO_CODEC,
	ERROR_COULD_NOT_OPEN_AUDIO_CODEC,
	ERROR_COULD_NOT_PREPARE_RGB_QUEUE,
	ERROR_COULD_NOT_PREPARE_AUDIO_PACKETS_QUEUE,
	ERROR_COULD_NOT_PREPARE_VIDEO_PACKETS_QUEUE,

	ERROR_WHILE_DUPLICATING_FRAME,

	ERROR_WHILE_DECODING_VIDEO,
	ERROR_COULD_NOT_RESAMPLE_FRAME,
	ERROR_WHILE_ALLOCATING_AUDIO_SAMPLE,
	ERROR_WHILE_DECODING_AUDIO_FRAME,
	ERROR_NOT_CREATED_AUDIO_TRACK,
	ERROR_NOT_CREATED_AUDIO_TRACK_GLOBAL_REFERENCE,
	ERROR_COULD_NOT_INIT_SWR_CONTEXT,
	ERROR_NOT_CREATED_AUDIO_SAMPLE_BYTE_ARRAY,
	ERROR_PLAYING_AUDIO,
	ERROR_WHILE_LOCING_BITMAP,

	ERROR_COULD_NOT_JOIN_PTHREAD,
	ERROR_COULD_NOT_INIT_PTHREAD_ATTR,
	ERROR_COULD_NOT_CREATE_PTHREAD,
	ERROR_COULD_NOT_DESTROY_PTHREAD_ATTR,
	ERROR_COULD_NOT_ALLOCATE_MEMORY,
};

enum DecodeCheckMsg {
	DECODE_CHECK_MSG_STOP = 0, DECODE_CHECK_MSG_FLUSH,
};

enum ReadFromStreamCheckMsg {
	READ_FROM_STREAM_CHECK_MSG_STOP = 0, READ_FROM_STREAM_CHECK_MSG_SEEK,
};

enum RenderCheckMsg {
	RENDER_CHECK_MSG_INTERRUPT = 0, RENDER_CHECK_MSG_FLUSH,
};
// FFmpegStreamInfo
static char *stream_info_class_path_name = "net/uplayer/ffmpeg/FFmpegStreamInfo";
static JavaMethod steram_info_set_metadata = {"setMetadata", "(Ljava/util/Map;)V"};
static JavaMethod steram_info_set_media_type_internal = {"setMediaTypeInternal", "(I)V"};
static JavaMethod stream_info_set_stream_number = {"setStreamNumber", "(I)V"};

// Set
static char *set_class_path_name = "java/util/Set";
static JavaMethod set_iterator = {"iterator", "()Ljava/util/Iterator;"};

// Iterator
static char *iterator_class_path_name = "java/util/Iterator";
static JavaMethod iterator_next = {"next", "()Ljava/lang/Object;"};
static JavaMethod iterator_has_next = {"hasNext", "()Z"};

static const struct {
    const char *name;
    int         nb_channels;
    uint64_t     layout;
} channel_android_layout_map[] = {
    { "mono",        1,  AV_CH_LAYOUT_MONO },
    { "stereo",      2,  AV_CH_LAYOUT_STEREO },
    { "2.1",         3,  AV_CH_LAYOUT_2POINT1 },
    { "4.0",         4,  AV_CH_LAYOUT_4POINT0 },
    { "4.1",         5,  AV_CH_LAYOUT_4POINT1 },
    { "5.1",         6,  AV_CH_LAYOUT_5POINT1_BACK },
    { "6.0",         6,  AV_CH_LAYOUT_6POINT0 },
    { "7.0(front)",  7,  AV_CH_LAYOUT_7POINT0_FRONT },
    { "7.1",         8,  AV_CH_LAYOUT_7POINT1 },
};

// FFmpegPlayer
static char *player_class_path_name = "net/uplayer/ffmpeg/FFmpegPlayer";
static JavaField player_m_native_player = {"mNativePlayer", "I"};
static JavaMethod player_on_update_time = {"onUpdateTime","(IIZ)V"};
static JavaMethod player_prepare_audio_track = {"prepareAudioTrack", "(II)Landroid/media/AudioTrack;"};
static JavaMethod player_prepare_frame = {"prepareFrame", "(II)Landroid/graphics/Bitmap;"};
static JavaMethod player_set_stream_info = {"setStreamsInfo", "([Lnet/uplayer/ffmpeg/FFmpegStreamInfo;)V"};

// AudioTrack
static char *android_track_class_path_name = "android/media/AudioTrack";
static JavaMethod audio_track_write = {"write", "([BII)I"};
static JavaMethod audio_track_pause = {"pause", "()V"};
static JavaMethod audio_track_play = {"play", "()V"};
static JavaMethod audio_track_flush = {"flush", "()V"};
static JavaMethod audio_track_stop = {"stop", "()V"};
static JavaMethod audio_track_get_channel_count = {"getChannelCount", "()I"};
static JavaMethod audio_track_get_sample_rate = {"getSampleRate", "()I"};

// Player
int jni_player_init(JNIEnv *env, jobject thiz);
void jni_player_dealloc(JNIEnv *env, jobject thiz);
void jni_player_seek(JNIEnv *env, jobject thiz, jint position);
void jni_player_pause(JNIEnv *env, jobject thiz);
void jni_player_resume(JNIEnv *env, jobject thiz);
int jni_player_set_data_source(JNIEnv *env, jobject thiz, jstring string,
		jobject dictionary, int video_stream_no, int audio_stream_no,
		int subtitle_stream_no);
void jni_player_stop(JNIEnv *env, jobject thiz);
void jni_player_render_frame_start(JNIEnv *env, jobject thiz);
void jni_player_render_frame_stop(JNIEnv *env, jobject thiz);
jobject jni_player_render_frame(JNIEnv *env, jobject thiz);
void jni_player_release_frame (JNIEnv *env, jobject thiz);
int jni_player_get_video_duration(JNIEnv *env, jobject thiz);

static JNINativeMethod player_methods[] = {
	{"initNative", "()I", (void*) jni_player_init},
	{"deallocNative", "()V", (void*) jni_player_dealloc},
	{"seekNative", "(I)V", (void*) jni_player_seek},
	{"pauseNative", "()V", (void*) jni_player_pause},
	{"resumeNative", "()V", (void*) jni_player_resume},
	{"setDataSourceNative", "(Ljava/lang/String;Ljava/util/Map;III)I", (void*) jni_player_set_data_source},
	{"stopNative", "()V", (void*) jni_player_stop},
	{"renderFrameStart", "()V", (void*) jni_player_render_frame_start},
	{"renderFrameStop", "()V", (void*) jni_player_render_frame_stop},
	{"renderFrameNative", "()Landroid/graphics/Bitmap;", (void*) jni_player_render_frame},
	{"releaseFrame", "()V", (void*) jni_player_release_frame},
	{"getVideoDurationNative", "()I", (void*) jni_player_get_video_duration},
};

#endif
