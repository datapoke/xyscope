// use pipewire to read the monitor stream
// and print samples to stdout

#include <iostream>
#include <string>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include <unistd.h>

using namespace std;

static void on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
    if (state == PW_STREAM_STATE_ERROR)
        cerr << "stream error: " << error << endl;
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    if (param == NULL || id != SPA_PARAM_Format)
        return;

    struct spa_audio_info_raw info;
    if (spa_format_audio_raw_parse(param, &info) < 0)
        return;

    cout << "format changed: " << info.format << " " << info.rate << " " << info.channels << endl;
}

static void on_process(void *data)
{
    struct pw_stream *stream = (struct pw_stream *)data;
    struct pw_buffer *buf;

    if ((buf = pw_stream_dequeue_buffer(stream)) <= 0)
        return;

    struct spa_data *d = &buf->buffer->datas[0];
    if (d->type == SPA_DATA_MemPtr)
    {
        // write samples to stdout
        write(STDOUT_FILENO, d->data, d->maxsize);
    }

    pw_stream_queue_buffer(stream, buf);
}

int main(int argc, char *argv[])
{
    struct pw_loop *loop;
    struct pw_context *context;
    struct pw_stream *stream;
    struct spa_pod_builder b = {0};
    uint8_t buffer[1024];
    struct spa_pod_frame f[2];
    const struct spa_pod *params[1];
    struct pw_properties *props;
    struct pw_stream_events events = {
        PW_VERSION_STREAM_EVENTS,
        .state_changed = on_state_changed,
        .param_changed = on_param_changed,
        .process = on_process,
    };
    uint32_t flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS;

    // create a new loop
    loop = pw_loop_new(NULL);

    // get context
    context = pw_context_new(loop, NULL, 0);

    // create a new context
    pw_context_get_main_loop(context);

    // set properties
    props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Monitor", NULL);

    // create a new stream
    stream = pw_stream_new_simple(loop, "monitor", props, &events, NULL);

    // build the params
    spa_pod_builder_init(&b, buffer, sizeof(buffer));
    spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
    spa_pod_builder_add(&b, SPA_PARAM_BUFFERS_size, SPA_POD_Int(1024), 0);
    spa_pod_builder_add(&b, SPA_PARAM_BUFFERS_stride, SPA_POD_Int(2), 0);
    spa_pod_builder_add(&b, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, 32), 0);
    spa_pod_builder_pop(&b, &f[0]);

    spa_pod_builder_push_object(&b, &f[1], SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
    spa_pod_builder_add(&b, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), 0);
    spa_pod_builder_add(&b, SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)), 0);
    spa_pod_builder_pop(&b, &f[1]);

    params[0] = (struct spa_pod *)spa_pod_builder_pop(&b, &f[0]);

    // set the params
    pw_stream_add_listener(stream, NULL, &events, stream);
    pw_stream_connect(stream, PW_DIRECTION_INPUT, 0, (pw_stream_flags) flags, params, 1);

    // run the loop
    pw_loop_enter(loop);

    // destroy the stream
    pw_stream_destroy(stream);

    // destroy the context
    pw_context_destroy(context);

    // destroy the loop
    pw_loop_destroy(loop);

    return 0;
}