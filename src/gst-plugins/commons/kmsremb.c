/*
 * (C) Copyright 2014 Kurento (http://kurento.org/)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "kmsremb.h"
#include "kmsrtcp.h"
#include "constants.h"

#define GST_CAT_DEFAULT kmsutils
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);
#define GST_DEFAULT_NAME "kmsremb"

#define REMB_MIN 30000          /* bps */
#define REMB_MAX 2000000        /* bps */

#define KMS_REMB_REMOTE "kms-remb-remote"
G_DEFINE_QUARK (KMS_REMB_REMOTE, kms_remb_remote);

#define DEFAULT_REMB_PACKETS_RECV_INTERVAL_TOP 100
#define DEFAULT_REMB_EXPONENTIAL_FACTOR 0.04
#define DEFAULT_REMB_LINEAL_FACTOR_MIN 50       /* bps */
#define DEFAULT_REMB_LINEAL_FACTOR_GRADE ((60 * RTCP_MIN_INTERVAL)/ 1000)       /* Reach last top bitrate in 60secs aprox. */
#define DEFAULT_REMB_DECREMENT_FACTOR 0.5
#define DEFAULT_REMB_THRESHOLD_FACTOR 0.8
#define DEFAULT_REMB_UP_LOSSES 12       /* 4% losses */

#define REMB_MAX_FACTOR_INPUT_BR 2

static void
kms_remb_base_destroy (KmsRembBase * self)
{
  g_signal_handler_disconnect (self->rtpsess, self->signal_id);
  self->signal_id = 0;
  g_object_set_qdata (self->rtpsess, kms_remb_remote_quark (), NULL);
  g_clear_object (&self->rtpsess);
  g_rec_mutex_clear (&self->mutex);
  g_hash_table_unref (self->remb_stats);
}

static void
kms_remb_base_create (KmsRembBase * self, GObject * rtpsess)
{
  self->rtpsess = g_object_ref (rtpsess);
  g_rec_mutex_init (&self->mutex);
  self->remb_stats = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      (GDestroyNotify) kms_utils_destroy_guint);
}

static void
kms_remb_base_update_stats (KmsRembBase * self, guint ssrc, guint bitrate)
{
  guint *value;

  KMS_REMB_BASE_LOCK (self);

  if (g_hash_table_contains (self->remb_stats, GUINT_TO_POINTER (ssrc))) {
    value =
        (guint *) g_hash_table_lookup (self->remb_stats, GUINT_TO_POINTER (ssrc));
  } else {
    value = g_slice_new0 (guint);
    g_hash_table_insert (self->remb_stats, GUINT_TO_POINTER (ssrc), value);
  }

  *value = bitrate;

  KMS_REMB_BASE_UNLOCK (self);
}

/* KmsRembLocal begin */

typedef struct _KmsRlRemoteSession
{
  GObject *rtpsess; // RTPSession* from GstRtpBin->GstRtpSession
  guint ssrc;

  guint64 last_packets_received_expected;
} KmsRlRemoteSession;

static KmsRlRemoteSession *
kms_rl_remote_session_create (GObject * rtpsess, guint ssrc)
{
  KmsRlRemoteSession *self = g_slice_new0 (KmsRlRemoteSession);

  self->rtpsess = g_object_ref (rtpsess);
  self->ssrc = ssrc;

  return self;
}

static void
kms_rl_remote_session_destroy (KmsRlRemoteSession * self)
{
  g_clear_object (&self->rtpsess);
  g_slice_free (KmsRlRemoteSession, self);
}

typedef struct _GetRtpSessionsInfo
{
  guint count;
  guint64 bitrate;
  guint fraction_lost_accumulative;     /* the sum of all sessions, it should be normalized */
  guint64 packets_received_expected_interval_accumulative;
  guint64 octets_received;
  guint64 packets_received;
} GetRtpSessionsInfo;

static void
kms_rl_remote_session_get_sessions_info (KmsRlRemoteSession * self,
    GetRtpSessionsInfo * data)
{
  GST_DEBUG_OBJECT (self->rtpsess, "REMB: Getting stats data from session sources");

  // Property RTPSession::sources, doc: GStreamer/rtpsession.c
  GValueArray *arr = NULL;
  GValue *val;
  guint i;

  g_object_get (self->rtpsess, "sources", &arr, NULL);
  if (!arr) {
    GST_ERROR_OBJECT (self->rtpsess, "REMB: Session lacks array of sources");
    return;
  }

  // Check that the SSRC which was announced over SDP
  // coincides with each source's SSRC in this session.
  for (i = 0; i < arr->n_values; i++) {
    GObject *rtpsource;
    guint ssrc;

    // FIXME 'g_value_array_get_nth' is deprecated: Use 'GArray' instead
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    // RTPSource*
    val = g_value_array_get_nth (arr, i);
    #pragma GCC diagnostic pop

    rtpsource = g_value_get_object (val);

    // Property RTPSource::ssrc, doc: GStreamer/rtpsource.c
    g_object_get (rtpsource, "ssrc", &ssrc, NULL);
    GST_TRACE_OBJECT (self->rtpsess, "REMB: Session source index: %u SSRC: %u", i, ssrc);

    // Each session has a minimum of 2 SSRCs: the sender's and the receiver's;
    // here we're looking for stats from the sender which had its SSRC registered
    // for congestion control (by providing it in the SDP negotiation).
    if (ssrc != self->ssrc) {
      GST_DEBUG_OBJECT (self->rtpsess, "REMB: Source SSRC (%u) doesn't match SDP SSRC (%u)", ssrc, self->ssrc);
      continue;
    }

    GST_DEBUG_OBJECT (self->rtpsess, "REMB: SSRC match! Session source index: %u SSRC: %u", i, ssrc);

    // Property RTPSource::stats, doc: GStreamer/rtpsource.c
    GstStructure *s;
    g_object_get (rtpsource, "stats", &s, NULL);
    GST_TRACE_OBJECT (rtpsource, "stats: %" GST_PTR_FORMAT, s);

    guint64 bitrate = 0, octets_received = 0, packets_received = 0;
    gint packets_lost=0;
    guint fraction_lost = 0;
    if (!gst_structure_get_uint64 (s, "bitrate", &bitrate)) {
      GST_ERROR_OBJECT (rtpsource, "REMB: RTPSource stats lack property 'bitrate'");
    }
    if (!gst_structure_get_uint64 (s, "octets-received", &octets_received)) {
      GST_ERROR_OBJECT (rtpsource, "REMB: RTPSource stats lack property 'octets-received'");
    }
    if (!gst_structure_get_uint64 (s, "packets-received", &packets_received)) {
      GST_ERROR_OBJECT (rtpsource, "REMB: RTPSource stats lack property 'packets-received'");
    }
    if (!gst_structure_get_int    (s, "packets-lost", &packets_lost)) {
      GST_ERROR_OBJECT (rtpsource, "REMB: RTPSource stats lack property 'packets-lost'");
    }
    if (!gst_structure_get_uint   (s, "sent-rb-fractionlost", &fraction_lost)) {
      GST_ERROR_OBJECT (rtpsource, "REMB: RTPSource stats lack property 'sent-rb-fractionlost'");
    }
    gst_structure_free (s);

    guint64 packets_received_expected =
        packets_received + packets_lost;
    guint64 packets_received_expected_interval =
        packets_received_expected - self->last_packets_received_expected;

    self->last_packets_received_expected = packets_received_expected;

    data->bitrate += bitrate;
    data->fraction_lost_accumulative +=
        fraction_lost * packets_received_expected_interval;
    data->packets_received_expected_interval_accumulative +=
        packets_received_expected_interval;
    data->octets_received += octets_received;
    data->packets_received += packets_received;
    data->count++;

    GST_TRACE_OBJECT (rtpsource,
        "packets_received: %" G_GUINT64_FORMAT ", packets_lost: %"
        G_GUINT32_FORMAT ", packets_received_expected_interval: %"
        G_GUINT64_FORMAT
        ", packets_received_expected_interval_accumulative: %"
        G_GUINT64_FORMAT, packets_received, packets_lost,
        packets_received_expected_interval,
        data->packets_received_expected_interval_accumulative);

    break;
  }

  // FIXME 'g_value_array_free' is deprecated: Use 'GArray' instead
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  g_value_array_free (arr);
  #pragma GCC diagnostic pop
}

static gboolean
kms_remb_local_get_video_recv_info (KmsRembLocal * self,
    guint64 * bitrate, guint * fraction_lost, guint64 * packets_rcv_interval)
{
  GetRtpSessionsInfo data;
  GstClockTime current_time;

  if (!KMS_REMB_BASE (self)->rtpsess) {
    GST_WARNING_OBJECT (self, "KmsRembLocal lacks a session object");
    return FALSE;
  }

  data.count = 0;
  data.bitrate = 0;
  data.fraction_lost_accumulative = 0;
  data.packets_received_expected_interval_accumulative = 0;
  data.octets_received = 0;
  data.packets_received = 0;

  //J REVIEW - Is it really possible to have more than 1 session in self->remote_sessions?
  g_slist_foreach (self->remote_sessions,
                   (GFunc) kms_rl_remote_session_get_sessions_info,
                   &data);

  if (data.count == 0) {
    GST_DEBUG_OBJECT (KMS_REMB_BASE (self)->rtpsess,
        "No stats: No SSRC match for this KmsRembLocal");
    return FALSE;
  }
  if (data.packets_received_expected_interval_accumulative == 0) {
    GST_DEBUG_OBJECT (KMS_REMB_BASE (self)->rtpsess,
        "No stats: No packets received yet");
    return FALSE;
  }
  GST_DEBUG_OBJECT (KMS_REMB_BASE (self)->rtpsess,
        "Stats sources count: %u", data.count);
  GST_DEBUG_OBJECT (KMS_REMB_BASE (self)->rtpsess,
        "Stats packets count: %lu", data.packets_received_expected_interval_accumulative);

  current_time = kms_utils_get_time_nsecs ();

  /* Normalize fraction_lost */
  *fraction_lost =
      data.fraction_lost_accumulative /
      data.packets_received_expected_interval_accumulative;

  *bitrate = data.bitrate;
  if (self->last_time != 0) {
    GstClockTime elapsed = current_time - self->last_time;
    guint64 bytes_handled = data.octets_received - self->last_octets_received;

    *bitrate = gst_util_uint64_scale (bytes_handled, 8 * GST_SECOND, elapsed);
    GST_TRACE_OBJECT (KMS_REMB_BASE (self)->rtpsess,
        "Elapsed %" G_GUINT64_FORMAT " bytes %" G_GUINT64_FORMAT ", rate %"
        G_GUINT64_FORMAT, elapsed, bytes_handled, *bitrate);
  }

  self->last_time = current_time;
  self->last_octets_received = data.octets_received;

  *packets_rcv_interval = data.packets_received - self->last_packets_received;
  self->last_packets_received = data.packets_received;

  return TRUE;
}

static gboolean
kms_remb_local_update (KmsRembLocal * self)
{
  guint64 bitrate, packets_rcv_interval;
  guint fraction_lost, packets_rcv_interval_top;

  if (!kms_remb_local_get_video_recv_info (self,
      &bitrate, &fraction_lost, &packets_rcv_interval)) {
    return FALSE;
  }

  if (!self->probed) {
    if (bitrate == 0) {
      GST_WARNING_OBJECT (KMS_REMB_BASE (self)->rtpsess,
          "No probe, and bitrate == 0");
      return FALSE;
    }

    self->remb = bitrate;
    self->probed = TRUE;
  }

  packets_rcv_interval_top =
      MAX (self->packets_recv_interval_top, packets_rcv_interval);
  self->fraction_lost_record =
      (self->fraction_lost_record * (packets_rcv_interval_top -
          packets_rcv_interval) +
      fraction_lost * packets_rcv_interval) / packets_rcv_interval_top;
  self->max_br = MAX (self->max_br, bitrate);

  if (self->avg_br == 0) {
    self->avg_br = bitrate;
  } else {
    self->avg_br = (self->avg_br * 7 + bitrate) / 8;
  }

  GST_TRACE_OBJECT (KMS_REMB_BASE (self)->rtpsess,
      "packets_rcv_interval: %" G_GUINT64_FORMAT ", fraction_lost: %"
      G_GUINT32_FORMAT ", fraction_lost_record: %" G_GUINT64_FORMAT,
      packets_rcv_interval, fraction_lost, self->fraction_lost_record);

  if (self->fraction_lost_record == 0) {
    gint remb_base, remb_new;

    remb_base = MAX (self->remb, self->max_br);

    if (remb_base < self->threshold) {
      GST_TRACE_OBJECT (KMS_REMB_BASE (self)->rtpsess,
          "A.1) Exponential (%f)", self->exponential_factor);
      remb_new = remb_base * (1 + self->exponential_factor);
    } else {
      GST_TRACE_OBJECT (KMS_REMB_BASE (self)->rtpsess,
          "A.2) Lineal (%" G_GUINT32_FORMAT ")", self->lineal_factor);
      remb_new = remb_base + self->lineal_factor;
    }

    remb_new = MIN (remb_new, self->max_br * REMB_MAX_FACTOR_INPUT_BR);
    self->remb = MAX (self->remb, remb_new);
  } else {
    gint remb_base, lineal_factor_new;

    remb_base = MAX (self->remb, self->avg_br);
    self->threshold = remb_base * self->threshold_factor;
    lineal_factor_new = (remb_base - self->threshold) / self->lineal_factor_grade;
    self->lineal_factor = MAX (self->lineal_factor_min, lineal_factor_new);

    if (self->fraction_lost_record < self->up_losses) {
      GST_TRACE_OBJECT (KMS_REMB_BASE (self)->rtpsess, "B) Assumable losses");

      self->remb = MIN (self->remb, self->max_br);
    }
    else {
      GST_TRACE_OBJECT (KMS_REMB_BASE (self)->rtpsess, "C) Too many losses");

      self->remb = remb_base * self->decrement_factor;
      self->fraction_lost_record = 0;
      self->max_br = 0;
      self->avg_br = 0;
    }
  }

  if (self->max_bw > 0) {
    self->remb = MIN (self->remb, self->max_bw * 1000);
  }

  GST_TRACE_OBJECT (KMS_REMB_BASE (self)->rtpsess,
      "REMB: %" G_GUINT32_FORMAT ", TH: %" G_GUINT32_FORMAT
      ", fraction_lost: %d, fraction_lost_record: %" G_GUINT64_FORMAT
      ", bitrate: %" G_GUINT64_FORMAT "," " max_br: %" G_GUINT32_FORMAT
      ", avg_br: %" G_GUINT32_FORMAT, self->remb, self->threshold, fraction_lost,
      self->fraction_lost_record, bitrate, self->max_br, self->avg_br);

  return TRUE;
}

typedef struct _AddSsrcsData
{
  KmsRembLocal *rl;
  KmsRTCPPSFBAFBREMBPacket *remb_packet;
} AddSsrcsData;

static void
add_ssrcs (KmsRlRemoteSession * rlrs, AddSsrcsData * data)
{
  KmsRembBase *rb = KMS_REMB_BASE (data->rl);

  data->remb_packet->ssrcs[data->remb_packet->n_ssrcs] = rlrs->ssrc;
  data->remb_packet->n_ssrcs++;

  GST_TRACE_OBJECT (rb->rtpsess, "REMB: Sending (bitrate: %" G_GUINT32_FORMAT
      ", ssrc: %" G_GUINT32_FORMAT ")", data->remb_packet->bitrate, rlrs->ssrc);

  kms_remb_base_update_stats (rb, rlrs->ssrc, data->remb_packet->bitrate);
}

// Signal "RTPSession::on-sending-rtcp" doc: GStreamer/rtpsession.c
static gboolean
kms_remb_local_on_sending_rtcp (GObject *rtpsession,
    GstBuffer *buffer, gboolean is_early, KmsRembLocal *self)
{
  gboolean ret = FALSE;
  GstClockTime current_time, elapsed;
  KmsRTCPPSFBAFBREMBPacket remb_packet;
  GstRTCPBuffer rtcp = {0,};
  GstRTCPPacket packet;
  guint packet_ssrc;
  AddSsrcsData data;

  GST_DEBUG_OBJECT (rtpsession, "REMB: Signal 'on-sending-rtcp'");

  current_time = kms_utils_get_time_nsecs ();
  elapsed = current_time - self->last_sent_time;
  if (self->last_sent_time != 0 && (elapsed < REMB_MAX_INTERVAL * GST_MSECOND)) {
    GST_DEBUG_OBJECT (rtpsession, "REMB: Not sending, interval < %u ms", REMB_MAX_INTERVAL);
    return ret;
  }

  if (!gst_rtcp_buffer_map (buffer, GST_MAP_READWRITE, &rtcp)) {
    GST_WARNING_OBJECT (rtpsession, "REMB: Cannot map RTCP buffer");
    return ret;
  }

  if (!gst_rtcp_buffer_add_packet (&rtcp, GST_RTCP_TYPE_PSFB, &packet)) {
    GST_WARNING_OBJECT (rtpsession, "REMB: Cannot add RTCP packet");
    goto end;
  }

  // Update the REMB bitrate estimations
  if (!kms_remb_local_update (self)) {
    GST_DEBUG_OBJECT (rtpsession, "REMB: Cannot update stats");
    goto end;
  }

  const guint32 old_bitrate = self->remb_sent;
  guint32 new_bitrate = self->remb;

  if (self->event_manager != NULL) {
    guint remb_local_max;

    remb_local_max = kms_utils_remb_event_manager_get_min (self->event_manager);
    if (remb_local_max > 0) {
      GST_TRACE_OBJECT (rtpsession, "REMB: Local max: %" G_GUINT32_FORMAT,
          remb_local_max);
      new_bitrate = MIN (new_bitrate, remb_local_max);
    }
  }

  if (self->min_bw > 0) {
    new_bitrate = MAX (new_bitrate, self->min_bw * 1000);
  }

  new_bitrate = MAX (new_bitrate, REMB_MIN);

  if (old_bitrate != new_bitrate) {
    GST_INFO_OBJECT (rtpsession, "REMB: Send local bitrate estimation: %u",
        new_bitrate);
    self->remb_sent = new_bitrate;
  }

  remb_packet.bitrate = new_bitrate;
  remb_packet.n_ssrcs = 0;
  data.rl = self;
  data.remb_packet = &remb_packet;
  g_slist_foreach (self->remote_sessions, (GFunc) add_ssrcs, &data);

  g_object_get (rtpsession, "internal-ssrc", &packet_ssrc, NULL);
  if (!kms_rtcp_psfb_afb_remb_marshall_packet (&packet, &remb_packet,
      packet_ssrc)) {
    gst_rtcp_packet_remove (&packet);
  }

  self->last_sent_time = current_time;
  ret = TRUE;

end:
  gst_rtcp_buffer_unmap (&rtcp);
  return ret;
}

void
kms_remb_local_destroy (KmsRembLocal * self)
{
  if (self == NULL) {
    return;
  }

  if (self->event_manager != NULL) {
    kms_utils_remb_event_manager_destroy (self->event_manager);
  }

  g_slist_free_full (self->remote_sessions,
      (GDestroyNotify) kms_rl_remote_session_destroy);
  kms_remb_base_destroy (KMS_REMB_BASE (self));

  g_slice_free (KmsRembLocal, self);
}

KmsRembLocal *
kms_remb_local_create (GObject * rtpsession, guint min_bw, guint max_bw)
{
  KmsRembLocal *self = g_slice_new0 (KmsRembLocal);

  self->base.signal_id = g_signal_connect (rtpsession, "on-sending-rtcp",
      G_CALLBACK (kms_remb_local_on_sending_rtcp), self);

  kms_remb_base_create (KMS_REMB_BASE (self), rtpsession);

  self->min_bw = min_bw;
  self->max_bw = max_bw;

  self->probed = FALSE;
  self->remb = REMB_MAX;
  self->remb_sent = REMB_MAX;
  self->threshold = REMB_MAX;
  self->lineal_factor = DEFAULT_REMB_LINEAL_FACTOR_MIN;

  self->packets_recv_interval_top = DEFAULT_REMB_PACKETS_RECV_INTERVAL_TOP;
  self->exponential_factor = DEFAULT_REMB_EXPONENTIAL_FACTOR;
  self->lineal_factor_min = DEFAULT_REMB_LINEAL_FACTOR_MIN;
  self->lineal_factor_grade = DEFAULT_REMB_LINEAL_FACTOR_GRADE;
  self->decrement_factor = DEFAULT_REMB_DECREMENT_FACTOR;
  self->threshold_factor = DEFAULT_REMB_THRESHOLD_FACTOR;
  self->up_losses = DEFAULT_REMB_UP_LOSSES;

  return self;
}

void
kms_remb_local_add_remote_session (KmsRembLocal * rl, GObject * rtpsess,
    guint ssrc)
{
  KmsRlRemoteSession *rlrs = kms_rl_remote_session_create (rtpsess, ssrc);

  rl->remote_sessions = g_slist_append (rl->remote_sessions, rlrs);
}

void
kms_remb_local_set_params (KmsRembLocal * rl, GstStructure * params)
{
  gfloat auxf;
  gint auxi;
  gboolean is_set;

  is_set =
      gst_structure_get (params, "packets-recv-interval-top", G_TYPE_INT,
      &auxi, NULL);
  if (is_set) {
    if (auxi <= 0) {
      GST_WARNING
          ("'packets-recv-interval-top' must be greater than 0. Setting to 1.");
      auxi = 1;
    }

    rl->packets_recv_interval_top = auxi;
  }

  is_set =
      gst_structure_get (params, "exponential-factor", G_TYPE_FLOAT,
      &auxf, NULL);
  if (is_set) {
    rl->exponential_factor = auxf;
  }

  is_set =
      gst_structure_get (params, "lineal-factor-min", G_TYPE_INT, &auxi, NULL);
  if (is_set) {
    rl->lineal_factor_min = auxi;
  }

  is_set =
      gst_structure_get (params, "lineal-factor-grade", G_TYPE_INT,
      &auxi, NULL);
  if (is_set) {
    rl->lineal_factor_grade = auxi;
  }

  is_set =
      gst_structure_get (params, "decrement-factor", G_TYPE_FLOAT, &auxf, NULL);
  if (is_set) {
    rl->decrement_factor = auxf;
  }

  is_set =
      gst_structure_get (params, "threshold-factor", G_TYPE_FLOAT, &auxf, NULL);
  if (is_set) {
    rl->threshold_factor = auxf;
  }

  is_set = gst_structure_get (params, "up-losses", G_TYPE_INT, &auxi, NULL);
  if (is_set) {
    rl->up_losses = auxi;
  }
}

void
kms_remb_local_get_params (KmsRembLocal * rl, GstStructure ** params)
{
  gst_structure_set (*params,
      "packets-recv-interval-top", G_TYPE_INT, rl->packets_recv_interval_top,
      "exponential-factor", G_TYPE_FLOAT, rl->exponential_factor,
      "lineal-factor-min", G_TYPE_INT, rl->lineal_factor_min,
      "lineal-factor-grade", G_TYPE_FLOAT, rl->lineal_factor_grade,
      "decrement-factor", G_TYPE_FLOAT, rl->decrement_factor,
      "threshold-factor", G_TYPE_FLOAT, rl->threshold_factor,
      "up-losses", G_TYPE_INT, rl->up_losses, NULL);
}

/* KmsRembLocal end */

/* KmsRembRemote begin */

#define DEFAULT_REMB_ON_CONNECT 300000  /* bps */

static void
send_remb_event (KmsRembRemote * rm, guint bitrate, guint ssrc)
{
  GstEvent *event;
  guint br, min = 0, max = 0;

  /* TODO: use g_atomic */
  if (rm->pad_event == NULL) {
    return;
  }

  br = bitrate;

  if (rm->min_bw > 0) {
    min = rm->min_bw * 1000;
    br = MAX (br, min);
  }

  if (rm->max_bw > 0) {
    max = rm->max_bw * 1000;
    br = MIN (br, max);
  }

  GST_TRACE_OBJECT (KMS_REMB_BASE (rm)->rtpsess,
      "REMB: 'on-feedback-rtcp' send upstream event"
      ", bitrate: %" G_GUINT32_FORMAT
      ", ssrc: %" G_GUINT32_FORMAT
      ", range [%" G_GUINT32_FORMAT ", %" G_GUINT32_FORMAT "]"
      ", event bitrate: %" G_GUINT32_FORMAT, bitrate, ssrc, min, max, br);

  event = kms_utils_remb_event_upstream_new (br, ssrc);
  gst_pad_push_event (rm->pad_event, event);
}

static GstPadProbeReturn
send_remb_event_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  KmsRembRemote *rm = user_data;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

  if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS) {
    return GST_PAD_PROBE_OK;
  };

  send_remb_event (rm, rm->remb_on_connect, rm->local_ssrc);

  return GST_PAD_PROBE_REMOVE;
}

static void
kms_remb_remote_update (KmsRembRemote * rm,
    const KmsRTCPPSFBAFBREMBPacket * remb_packet)
{
  guint32 br_send;

  if (remb_packet->n_ssrcs == 0) {
    GST_WARNING_OBJECT (KMS_REMB_BASE (rm)->rtpsess,
        "REMB packet without any SSRC");
    return;
  } else if (remb_packet->n_ssrcs > 1) {
    GST_FIXME_OBJECT (KMS_REMB_BASE (rm)->rtpsess,
        "REMB packet with %" G_GUINT32_FORMAT " SSRCs."
        " A inconsistent management could take place", remb_packet->n_ssrcs);
  }

  br_send = remb_packet->bitrate;
  if (!rm->probed) {
    if ((remb_packet->bitrate < rm->remb_on_connect)
        && (remb_packet->bitrate >= rm->remb)) {
      GST_DEBUG_OBJECT (KMS_REMB_BASE (rm)->rtpsess,
          "Not probed: sending remb_on_connect value");
      br_send = rm->remb_on_connect;
      rm->remb = remb_packet->bitrate;
    } else {
      rm->probed = TRUE;
    }
  }

  GST_INFO_OBJECT (KMS_REMB_BASE (rm)->rtpsess,
      "REMB: Received remote bitrate estimation: %u, constrained to: %u",
      remb_packet->bitrate, br_send);

  send_remb_event (rm, br_send, remb_packet->ssrcs[0]);
  rm->remb = remb_packet->bitrate;
}

static void
kms_remb_remote_update_target_ssrcs_stats (KmsRembRemote * rm,
    KmsRTCPPSFBAFBREMBPacket * remb_packet)
{
  guint i;

  for (i = 0; i < remb_packet->n_ssrcs; i++) {
    kms_remb_base_update_stats (KMS_REMB_BASE (rm), remb_packet->ssrcs[i],
        remb_packet->bitrate);
  }
}

static void
process_psfb_afb (GObject * sess, guint ssrc, GstBuffer * fci_buffer)
{
  KmsRembRemote *rm;
  KmsRTCPPSFBAFBBuffer afb_buffer = { NULL, };
  KmsRTCPPSFBAFBPacket afb_packet;
  KmsRTCPPSFBAFBREMBPacket remb_packet;
  KmsRTCPPSFBAFBType type;

  if (!G_IS_OBJECT (sess)) {
    GST_WARNING ("Invalid session object");
    return;
  }

  rm = g_object_get_qdata (sess, kms_remb_remote_quark ());

  if (!rm) {
    GST_WARNING ("Invalid RembRemote");
    return;
  }

  if (!kms_rtcp_psfb_afb_buffer_map (fci_buffer, GST_MAP_READ, &afb_buffer)) {
    GST_WARNING_OBJECT (fci_buffer, "Buffer cannot be mapped");
    return;
  }

  if (!kms_rtcp_psfb_afb_get_packet (&afb_buffer, &afb_packet)) {
    GST_WARNING_OBJECT (fci_buffer, "Cannot get RTCP PSFB AFB packet");
    goto end;
  }

  type = kms_rtcp_psfb_afb_packet_get_type (&afb_packet);
  switch (type) {
    case KMS_RTCP_PSFB_AFB_TYPE_REMB:
      kms_rtcp_psfb_afb_remb_get_packet (&afb_packet, &remb_packet);
      kms_remb_remote_update (rm, &remb_packet);
      kms_remb_remote_update_target_ssrcs_stats (rm, &remb_packet);
      break;
    default:
      break;
  }

end:
  kms_rtcp_psfb_afb_buffer_unmap (&afb_buffer);
}

static void
kms_remb_remote_on_feedback_rtcp (GObject *rtpsession,
    guint type, guint fbtype, guint sender_ssrc, guint media_ssrc,
    GstBuffer *fci)
{
  GST_DEBUG_OBJECT (rtpsession, "REMB: Signal 'on-feedback-rtcp'");
  if (type == GST_RTCP_TYPE_PSFB
      && fbtype == GST_RTCP_PSFB_TYPE_AFB) {
    process_psfb_afb (rtpsession, sender_ssrc, fci);
  }
}

void
kms_remb_remote_destroy (KmsRembRemote * rm)
{
  if (rm == NULL) {
    return;
  }

  if (rm->pad_event != NULL) {
    g_object_unref (rm->pad_event);
  }

  kms_remb_base_destroy (KMS_REMB_BASE (rm));

  g_slice_free (KmsRembRemote, rm);
}

KmsRembRemote *
kms_remb_remote_create (GObject * rtpsession, guint local_ssrc,
    guint min_bw, guint max_bw, GstPad * pad)
{
  KmsRembRemote *self = g_slice_new0 (KmsRembRemote);

  g_object_set_qdata (rtpsession, kms_remb_remote_quark (), self);

  self->base.signal_id = g_signal_connect (rtpsession, "on-feedback-rtcp",
      G_CALLBACK (kms_remb_remote_on_feedback_rtcp), self);

  kms_remb_base_create (KMS_REMB_BASE (self), rtpsession);

  self->local_ssrc = local_ssrc;
  self->min_bw = min_bw;
  self->max_bw = max_bw;

  self->remb_on_connect = DEFAULT_REMB_ON_CONNECT;

  self->pad_event = g_object_ref (pad);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      send_remb_event_probe, self, NULL);

  return self;
}

void
kms_remb_remote_set_params (KmsRembRemote * rm, GstStructure * params)
{
  gint auxi;
  gboolean is_set;

  is_set =
      gst_structure_get (params, "remb-on-connect", G_TYPE_INT, &auxi, NULL);
  if (is_set) {
    rm->remb_on_connect = auxi;
  }
}

void
kms_remb_remote_get_params (KmsRembRemote * rm, GstStructure ** params)
{
  gst_structure_set (*params,
      "remb-on-connect", G_TYPE_INT, rm->remb_on_connect, NULL);
}

/* KmsRembRemote end */

static void init_debug (void) __attribute__ ((constructor));

static void
init_debug (void)
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_DEFAULT_NAME, 0,
      GST_DEFAULT_NAME);
}
