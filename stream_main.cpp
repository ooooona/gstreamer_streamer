#include <unistd.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
// gst
#include <gst/app/gstappsink.h>
#include <gst/gst.h>

using namespace rapidjson;

struct Camera {
    Camera()
        : id(0),
          fps(6),
          width(1280),
          height(720),
          address("rtsp://test:test@192.168.1.64:554") {}
    int id;
    int fps;
    int width;
    int height;
    std::string address;
};

enum class GSTCameraState {
    PLAY = 0,
    INIT = 1,
    OFFLINE = 2,
};

uint64_t g_upd_frame_timestamp = 0;
int g_retry_times = 0;
Camera g_camera;
GSTCameraState g_camera_state;

GstElement *g_pipeline;
GstBus *g_bus;
GstAppSink *g_appsink;

void open();
void close();
int build_pipeline();
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data);
static int send_frame(GstSample *gst_sample, const void *gst_map_data,
                      const guint &gst_map_size);
void check_gst_message_all();
void check_gst_message_once(GstMessage *msg);
const char *match_gst_status_string(GstStreamStatusType status);

void checkState();
uint64_t getNowTimestamp();

/************************************ gstreamer
 * ****************************************/
void open() {
    g_camera_state = GSTCameraState::INIT;
    g_upd_frame_timestamp = getNowTimestamp();
    build_pipeline();
    std::cout << "Play" << std::endl;
    gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
}

void close() {
    std::cout << "Close" << std::endl;
    g_camera_state = GSTCameraState::OFFLINE;
    // set state as NULL
    gst_element_set_state(g_pipeline, GST_STATE_NULL);
    // unref appsink
    gst_object_unref(G_OBJECT(g_appsink));
    // unref bus
    gst_object_unref(G_OBJECT(g_bus));
    // unref pipeline
    gst_object_unref(G_OBJECT(g_pipeline));
}

int build_pipeline() {
    std::string parse_str =
        std::string("rtspsrc location=" + g_camera.address + " protocols=tcp" +
                    " ! rtph264depay ! h264parse ! omxh264dec" +
                    " ! video/x-raw, format=NV12" +
                    " ! appsink sync=false async=false name=appsink" +
                    std::to_string(g_camera.id));
    std::cout << "Builds pipeline: " << parse_str << std::endl;

    GError *err = NULL;
    g_pipeline = gst_parse_launch(parse_str.c_str(), &err);
    if (err) {
        std::cout << "Parse pipeline Error: " << err->message << std::endl;
        return -1;
    }
    g_bus = gst_element_get_bus(g_pipeline);
    if (!(g_bus)) {
        std::cout << "Build bus Error" << std::endl;
        return -1;
    }
    std::string sink_name = "appsink" + std::to_string(g_camera.id);
    g_appsink = (GstAppSink *)gst_bin_get_by_name(GST_BIN(g_pipeline),
                                                  sink_name.c_str());
    if (!(g_appsink)) {
        std::cout << "Builds appsink Error" << std::endl;
        return -1;
    }

    gst_app_sink_set_emit_signals((GstAppSink *)g_appsink, true);
    gst_app_sink_set_drop((GstAppSink *)g_appsink, true);
    gst_app_sink_set_max_buffers((GstAppSink *)g_appsink, 1);

    GstAppSinkCallbacks ascb;
    std::memset(&ascb, 0, sizeof(GstAppSinkCallbacks));
    ascb.eos = NULL;
    ascb.new_preroll = NULL;
    ascb.new_sample = on_new_sample;

    gst_app_sink_set_callbacks((GstAppSink *)g_appsink, &ascb, NULL, NULL);
    return 0;
}

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
    check_gst_message_all();
    GstSample *gst_sample = gst_app_sink_pull_sample(g_appsink);
    if (!gst_sample) {
        std::cout << "GstSample is null" << std::endl;
        return GST_FLOW_ERROR;
    }

    GstBuffer *gst_buffer = gst_sample_get_buffer(gst_sample);
    if (!gst_buffer) {
        std::cout << "GstBuffer is null" << std::endl;
        gst_sample_unref(gst_sample);
        return GST_FLOW_ERROR;
    }

    GstMapInfo gst_map;
    if (!gst_buffer_map(gst_buffer, &gst_map, GST_MAP_READ)) {
        std::cout << "GstMapInfo failed" << std::endl;
        gst_sample_unref(gst_sample);
        return GST_FLOW_ERROR;
    }

    const void *gst_map_data = gst_map.data;
    const guint gst_map_size = gst_map.size;
    if (!gst_map_data || gst_map_size <= 0) {
        std::cout << "GstMapInfo's data is null" << std::endl;
        gst_buffer_unmap(gst_buffer, &gst_map);
        gst_sample_unref(gst_sample);
        return GST_FLOW_ERROR;
    }

    g_camera_state = GSTCameraState::PLAY;
    send_frame(gst_sample, gst_map_data, gst_map_size);
    g_upd_frame_timestamp = getNowTimestamp();

    gst_buffer_unmap(gst_buffer, &gst_map);
    gst_sample_unref(gst_sample);
    return GST_FLOW_OK;
}

static int send_frame(GstSample *gst_sample, const void *gst_map_data,
                      const guint &gst_map_size) {
    GstCaps *gst_caps = gst_sample_get_caps(gst_sample);
    if (!gst_caps) {
        std::cout << "GstBuffer has no capabilities" << std::endl;
        return -1;
    }

    GstStructure *gst_caps_struct = gst_caps_get_structure(gst_caps, 0);
    if (!gst_caps_struct) {
        std::cout << "GstCaps has no structure" << std::endl;
        return -1;
    }

    int frame_width = 0;
    int frame_height = 0;
    if (!gst_structure_get_int(gst_caps_struct, "width", &frame_width)) {
        std::cout << "GstCaps has no width" << std::endl;
        return -1;
    }
    if (!gst_structure_get_int(gst_caps_struct, "height", &frame_height)) {
        std::cout << "GstCaps has no height" << std::endl;
        return -1;
    }
    if (frame_width < 1 || frame_height < 1) {
        std::cout << "width: " << frame_width << " or height: " << frame_height
                  << " invalid" << std::endl;
        return -1;
    }

    // std::chrono::steady_clock::time_point pt1 =
    // std::chrono::steady_clock::now(); uint64_t now_timestamp =
    // getNowTimestamp();
    std::cout << "Put Frame!!!" << std::endl;
    // g_interface -> putFrame(
    //     FrameInfo(frame_width, frame_height, 24, g_camera.id, now_timestamp),
    //     (uint8_t*)gst_map_data
    // );
    // std::chrono::steady_clock::time_point pt2 =
    // std::chrono::steady_clock::now();
    return 0;
}

void check_gst_message_all() {
    while (true) {
        GstMessage *msg = gst_bus_pop(g_bus);
        if (!msg) {
            break;
        }
        check_gst_message_once(msg);
        gst_message_unref(msg);
    }
}

void check_gst_message_once(GstMessage *msg) {
    const char *obj_name = GST_OBJECT_NAME(msg->src);
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = nullptr;
            gchar *dbg_info = nullptr;
            gst_message_parse_error(msg, &err, &dbg_info);
            std::cout << "[GStreamer Error] [object=" << obj_name << "] [err={"
                      << err->message << "}]" << std::endl;
            if (dbg_info) {
                std::cout << "[GStreamer Debug] [debug_info=" << dbg_info << "]"
                          << std::endl;
            }
            g_error_free(err);
            g_free(dbg_info);
            break;
        }
        case GST_MESSAGE_EOS: {
            std::cout << "[GStreamer EOS] [object=" << std::string(obj_name)
                      << "]" << std::endl;
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_state, new_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state,
                                            nullptr);
            const char *old_state_name = gst_element_state_get_name(old_state);
            const char *new_state_name = gst_element_state_get_name(new_state);
            std::cout << "[GStreamer Change State] [object="
                      << std::string(obj_name) << "] "
                      << "[old_state=" << std::string(old_state_name) << "] "
                      << "[new_state=" << std::string(new_state_name) << "]"
                      << std::endl;
            break;
        }
        case GST_MESSAGE_STREAM_STATUS: {
            GstStreamStatusType status;
            gst_message_parse_stream_status(msg, &status, nullptr);
            const char *status_str = match_gst_status_string(status);
            std::cout << "[GStreamer Stream Status] [object="
                      << std::string(obj_name) << "] "
                      << "[status=" << std::string(status_str) << "]"
                      << std::endl;
            break;
        }
        case GST_MESSAGE_TAG: {
            GstTagList *tags = nullptr;
            gst_message_parse_tag(msg, &tags);
            gchar *tags_str = gst_tag_list_to_string(tags);
            g_free(tags_str);
            if (tags != NULL) {
                gst_tag_list_free(tags);
            }
            break;
        }
        default: {
            const char *msg_type =
                gst_message_type_get_name(GST_MESSAGE_TYPE(msg));
            std::cout << "[GStreamer Other Message] [object="
                      << std::string(obj_name)
                      << "] [msg_type=" << std::string(msg_type) << "]"
                      << std::endl;
            break;
        }
    }
}

const char *match_gst_status_string(GstStreamStatusType status) {
    switch (status) {
        case GST_STREAM_STATUS_TYPE_CREATE:
            return "CREATE";
        case GST_STREAM_STATUS_TYPE_ENTER:
            return "ENTER";
        case GST_STREAM_STATUS_TYPE_LEAVE:
            return "LEAVE";
        case GST_STREAM_STATUS_TYPE_DESTROY:
            return "DESTROY";
        case GST_STREAM_STATUS_TYPE_START:
            return "START";
        case GST_STREAM_STATUS_TYPE_PAUSE:
            return "PAUSE";
        case GST_STREAM_STATUS_TYPE_STOP:
            return "STOP";
    }
    return "UNKNOWN";
}

/************************************ camera monitor
 * ****************************************/
void checkState() {
    while (true) {
        usleep(500000);
        uint64_t now_timestamp = getNowTimestamp();
        std::cout << "..." << std::endl;
        std::cout << "upd_frame_timestamp: " << g_upd_frame_timestamp
                  << "; now_timestamp: " << now_timestamp
                  << "; duration: " << now_timestamp - g_upd_frame_timestamp
                  << std::endl;
        std::cout << "..." << std::endl;
        if (now_timestamp - g_upd_frame_timestamp > 12000 ||
            g_camera_state == GSTCameraState::OFFLINE) {
            ++g_retry_times;
            std::cout << "blocked or dead" << std::endl;
            close();
            usleep(1000);
            open();
            std::cout << "retry_times: " << g_retry_times << std::endl;
        } else {
            g_retry_times = 0;
        }
    }
}

/************************************ other
 * ****************************************/
uint64_t getNowTimestamp() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/************************************ main
 * ****************************************/
int main(int argc, char **argv) {
    // init camera
    g_camera.id = 1;
    g_camera.fps = 10;
    g_camera.width = 1920;
    g_camera.height = 1080;
    g_camera.address =
        "rtsp://test:test@192.168.1.64:554/cam/"
        "realmonitor?channel=1&subtype=0";

    // start gsteamer
    gst_init(0, NULL);
    open();
    usleep(300000);

    // check status
    checkState();
    return 0;
}