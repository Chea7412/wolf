#include <functional>
#include <helpers/logger.hpp>
#include <immer/array.hpp>
#include <immer/box.hpp>
#include <memory>
#include <moonlight/data-structures.hpp>
#include <streaming/data-structures.hpp>
#include <streaming/gst-plugin/gstrtpmoonlightpay_audio.hpp>
#include <streaming/gst-plugin/gstrtpmoonlightpay_video.hpp>
#include <streaming/streaming.hpp>

namespace streaming {

using gst_element_ptr = std::shared_ptr<GstElement>;
using gst_main_loop_ptr = std::shared_ptr<GMainLoop>;

/**
 * GStreamer needs to be initialised once per run
 * Call this method in your main.
 */
void init() {
  /* It is also possible to call the init function with two NULL arguments,
   * in which case no command line options will be parsed by GStreamer.
   */
  gst_init(nullptr, nullptr);

  GstPlugin *video_plugin = gst_plugin_load_by_name("rtpmoonlightpay_video");
  gst_element_register(video_plugin, "rtpmoonlightpay_video", GST_RANK_PRIMARY, gst_TYPE_rtp_moonlight_pay_video);

  GstPlugin *audio_plugin = gst_plugin_load_by_name("rtpmoonlightpay_audio");
  gst_element_register(audio_plugin, "rtpmoonlightpay_audio", GST_RANK_PRIMARY, gst_TYPE_rtp_moonlight_pay_audio);

  moonlight::fec::init();
}

static gboolean msg_handler(GstBus *bus, GstMessage *message, gpointer data) {
  auto loop = (GMainLoop *)data;
  switch (GST_MESSAGE_TYPE(message)) {
  case GST_MESSAGE_ERROR: {
    GError *err;
    gchar *debug;
    gst_message_parse_error(message, &err, &debug);
    logs::log(logs::error, "[GSTREAMER] Pipeline error: {}", err->message);
    g_error_free(err);
    g_free(debug);

    /* Terminate pipeline on error */
    g_main_loop_quit(loop);
    break;
  }
  case GST_MESSAGE_EOS:
    /* end-of-stream */
    g_main_loop_quit(loop);
    break;
  default:
    /* unhandled message */
    break;
  }

  /*
   * we want to be notified again the next time there is a message
   * on the bus, so returning TRUE (FALSE means we want to stop watching
   * for messages on the bus and our callback should not be called again)
   */
  return TRUE;
}

bool run_pipeline(
    const std::string &pipeline_desc,
    const std::function<immer::array<immer::box<dp::handler_registration>>(gst_element_ptr, gst_main_loop_ptr)>
        &on_pipeline_ready) {
  GError *error = nullptr;
  gst_element_ptr pipeline(gst_parse_launch(pipeline_desc.c_str(), &error), ::gst_object_unref);

  if (!pipeline) {
    logs::log(logs::error, "[GSTREAMER] Parse error: {}", error->message);
    g_error_free(error);
    return false;
  }

  /*
   * create a mainloop that runs/iterates the default GLib main context
   * (context NULL), in other words: makes the context check if anything
   * it watches for has happened. When a message has been posted on the
   * bus, the default main context will automatically call our
   * my_bus_callback() function to notify us of that message.
   */
  gst_main_loop_ptr loop(g_main_loop_new(nullptr, FALSE), ::g_main_loop_unref);

  /*
   * adds a watch for new message on our pipeline's message bus to
   * the default GLib main context, which is the main context that our
   * GLib main loop is attached to below
   */
  auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
  auto bus_watch_id = gst_bus_add_watch(bus, msg_handler, loop.get());
  gst_object_unref(bus);

  /* Let the calling thread set extra things */
  auto handlers = on_pipeline_ready(pipeline, loop);

  /* Set the pipeline to "playing" state*/
  gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);
  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(reinterpret_cast<GstBin *>(pipeline.get()),
                                    GST_DEBUG_GRAPH_SHOW_ALL,
                                    "pipeline-start");

  /* The main loop will be run until someone calls g_main_loop_quit() */
  g_main_loop_run(loop.get());

  /* Out of the main loop, clean up nicely */
  gst_element_set_state(pipeline.get(), GST_STATE_PAUSED);
  gst_element_set_state(pipeline.get(), GST_STATE_READY);
  gst_element_set_state(pipeline.get(), GST_STATE_NULL);
  g_source_remove(bus_watch_id);

  for (const auto &handler : handlers) {
    handler->unregister();
  }

  return true;
}

/**
 * Start VIDEO pipeline
 */
void start_streaming_video(immer::box<state::VideoSession> video_session, unsigned short client_port) {
  std::string color_range = (static_cast<int>(video_session->color_range) == static_cast<int>(state::JPEG)) ? "jpeg"
                                                                                                            : "mpeg2";
  std::string color_space;
  switch (static_cast<int>(video_session->color_space)) {
  case state::BT601:
    color_space = "bt601";
    break;
  case state::BT709:
    color_space = "bt709";
    break;
  case state::BT2020:
    color_space = "bt2020";
    break;
  }

  auto pipeline = fmt::format(video_session->gst_pipeline,
                              fmt::arg("width", video_session->display_mode.width),
                              fmt::arg("height", video_session->display_mode.height),
                              fmt::arg("fps", video_session->display_mode.refreshRate),
                              fmt::arg("bitrate", video_session->bitrate_kbps),
                              fmt::arg("client_port", client_port),
                              fmt::arg("client_ip", video_session->client_ip),
                              fmt::arg("payload_size", video_session->packet_size),
                              fmt::arg("fec_percentage", video_session->fec_percentage),
                              fmt::arg("min_required_fec_packets", video_session->min_required_fec_packets),
                              fmt::arg("slices_per_frame", video_session->slices_per_frame),
                              fmt::arg("color_space", color_space),
                              fmt::arg("color_range", color_range));

  run_pipeline(pipeline, [video_session](auto pipeline, auto loop) {
    /*
     * The force IDR event will be triggered by the control stream.
     * We have to pass this back into the gstreamer pipeline
     * in order to force the encoder to produce a new IDR packet
     */
    auto idr_handler = video_session->event_bus->register_handler<immer::box<ControlEvent>>(
        [sess_id = video_session->session_id, pipeline](immer::box<ControlEvent> ctrl_ev) {
          if (ctrl_ev->session_id == sess_id) {
            if (ctrl_ev->type == IDR_FRAME) {
              logs::log(logs::debug, "[GSTREAMER] Forcing IDR");

              gst_element_ptr moonlight_plugin(
                  gst_bin_get_by_name(reinterpret_cast<GstBin *>(pipeline.get()), "moonlight_pay"),
                  ::gst_object_unref);
              // Force IDR event, see: https://github.com/centricular/gstwebrtc-demos/issues/186
              auto gst_ev = gst_event_new_custom(
                  GST_EVENT_CUSTOM_UPSTREAM,
                  gst_structure_new("GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN, TRUE, NULL));

              gst_element_send_event(moonlight_plugin.get(), gst_ev);
            }
          }
        });

    auto terminate_handler = video_session->event_bus->register_handler<immer::box<TerminateEvent>>(
        [sess_id = video_session->session_id, loop](immer::box<TerminateEvent> term_ev) {
          if (term_ev->session_id == sess_id) {
            logs::log(logs::debug, "[GSTREAMER] Terminating video pipeline");
            g_main_loop_quit(loop.get());
          }
        });

    return immer::array<immer::box<dp::handler_registration>>{std::move(idr_handler), std::move(terminate_handler)};
  });
}

/**
 * Start AUDIO pipeline
 */
void start_streaming_audio(immer::box<state::AudioSession> audio_session, unsigned short client_port) {
  auto pipeline = fmt::format(audio_session->gst_pipeline,
                              fmt::arg("channels", audio_session->channels),
                              fmt::arg("bitrate", audio_session->bitrate),
                              fmt::arg("packet_duration", audio_session->packet_duration),
                              fmt::arg("aes_key", audio_session->aes_key),
                              fmt::arg("aes_iv", audio_session->aes_iv),
                              fmt::arg("encrypt", audio_session->encrypt_audio),
                              fmt::arg("client_port", client_port),
                              fmt::arg("client_ip", audio_session->client_ip));

  run_pipeline(pipeline, [audio_session](auto pipeline, auto loop) {
    auto terminate_handler = audio_session->event_bus->register_handler<immer::box<TerminateEvent>>(
        [sess_id = audio_session->session_id, loop](immer::box<TerminateEvent> term_ev) {
          if (term_ev->session_id == sess_id) {
            logs::log(logs::debug, "[GSTREAMER] Terminating audio pipeline");
            g_main_loop_quit(loop.get());
          }
        });

    return immer::array<immer::box<dp::handler_registration>>{std::move(terminate_handler)};
  });
}

} // namespace streaming