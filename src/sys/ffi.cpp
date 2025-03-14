
#include "libwebm/mkvmuxer/mkvmuxer.h"
#include "libwebm/mkvmuxer/mkvmuxertypes.h"
#include "libwebm/mkvmuxer/mkvmuxerutil.h"
#include "libwebm/mkvmuxer/mkvwriter.h"
#include "libwebm/mkvparser/mkvparser.h"
#include "libwebm/mkvparser/mkvreader.h"
#include "libwebm/common/webmids.h"

#include <stdint.h>
#include <assert.h>

extern "C" {
  enum class ResultCode: int32_t {
    Ok = 0,
    BadParam = -1,
    UnknownLibwebmError = -2,
  };

  using TrackNum = uint64_t;
  typedef mkvmuxer::IMkvWriter* MkvWriterPtr;

  struct FfiMkvWriter: public mkvmuxer::IMkvWriter {
  public:
    typedef bool (*WriteFun)(void*, const void*, size_t);
    typedef int64_t (*GetPositionFun)(void*);
    typedef bool (*SetPositionFun)(void*, uint64_t);
    typedef void (*ElementStartNotifyFun)(void*, uint64_t, int64_t);

    WriteFun              write_                = nullptr;
    GetPositionFun        get_position_         = nullptr;
    SetPositionFun        set_position_         = nullptr;
    ElementStartNotifyFun element_start_notify_ = nullptr;

    mutable void* user_data = nullptr;

    FfiMkvWriter() = default;
    virtual ~FfiMkvWriter() = default;

    mkvmuxer::int32 Write(const void* buf, uint32_t len) override final {
      assert(this->write_ != nullptr);

      return this->write_(this->user_data, buf, static_cast<size_t>(len)) ? 0 : 1;
    }
    mkvmuxer::int64 Position() const override final {
      assert(this->get_position_ != nullptr);

      return this->get_position_(this->user_data);
    }
    mkvmuxer::int32 Position(mkvmuxer::int64 pos) override final {
      if(this->set_position_ == nullptr) { return 1; }

      if(this->set_position_(this->user_data, pos)) {
        return 0;
      } else {
        return 1;
      }
    }
    bool Seekable() const override final {
      return this->set_position_ != nullptr;
    }
    void ElementStartNotify(mkvmuxer::uint64 element_id, mkvmuxer::int64 position) override final {
      if(this->element_start_notify_ == nullptr) { return; }

      this->element_start_notify_(this->user_data, element_id, position);
    }
  };

  MkvWriterPtr mux_new_writer(FfiMkvWriter::WriteFun write,
                              FfiMkvWriter::GetPositionFun get_position,
                              FfiMkvWriter::SetPositionFun set_position,
                              FfiMkvWriter::ElementStartNotifyFun element_start_notify,
                              void* user_data) {
    // Even for non-seekable streams, the writer will query the current position
    if(write == nullptr || get_position == nullptr) {
      return nullptr;
    }

    FfiMkvWriter* writer = new FfiMkvWriter;
    writer->write_ = write;
    writer->get_position_ = get_position;
    writer->set_position_ = set_position;
    writer->element_start_notify_ = element_start_notify;
    writer->user_data = user_data;


    return static_cast<MkvWriterPtr>(writer);
  }

  void mux_delete_writer(MkvWriterPtr writer) {
    delete static_cast<FfiMkvWriter*>(writer);
  }

  typedef mkvmuxer::Segment* MuxSegmentPtr;
  MuxSegmentPtr mux_new_segment() {
    return new mkvmuxer::Segment();
  }
  ResultCode mux_initialize_segment(MuxSegmentPtr segment, MkvWriterPtr writer) {
    bool success = segment->Init(writer);
    return success ? ResultCode::Ok : ResultCode::UnknownLibwebmError;
  }
  void mux_set_writing_app(MuxSegmentPtr segment, const char *name) {
    auto info = segment->GetSegmentInfo();
    info->set_writing_app(name);
  }
  ResultCode mux_finalize_segment(MuxSegmentPtr segment, uint64_t timeCodeDuration) {
    if (timeCodeDuration) {
      segment->set_duration(timeCodeDuration);
    }
    bool success = segment->Finalize();
    return success ? ResultCode::Ok : ResultCode::UnknownLibwebmError;
  }
  void mux_delete_segment(MuxSegmentPtr segment) {
    delete segment;
  }

  typedef mkvmuxer::Track* MuxTrackPtr;
  typedef mkvmuxer::VideoTrack* MuxVideoTrackPtr;
  typedef mkvmuxer::AudioTrack* MuxAudioTrackPtr;

  // audio
  const uint32_t OPUS_CODEC_ID = 0;
  const uint32_t VORBIS_CODEC_ID = 1;

  // video
  const uint32_t VP8_CODEC_ID = 0;
  const uint32_t VP9_CODEC_ID = 1;
  const uint32_t AV1_CODEC_ID = 2;

  ResultCode mux_segment_set_codec_private(MuxSegmentPtr segment, TrackNum track_num, const uint8_t *data, int len) {
    MuxTrackPtr track = segment->GetTrackByNumber(track_num);
    if (!track) { return ResultCode::BadParam; }
    if (!track->SetCodecPrivate(data, len)) { return ResultCode::UnknownLibwebmError; }
    return ResultCode::Ok;
  }

  ResultCode mux_segment_add_video_track(MuxSegmentPtr segment, const int32_t width,
                                               const int32_t height, const int32_t number,
                                               const uint32_t codec_id, TrackNum* track_num_out) {
    if(segment == nullptr || track_num_out == nullptr) { return ResultCode::BadParam; }

    const char* codec_id_str = nullptr;
    switch(codec_id) {
    case VP8_CODEC_ID: codec_id_str = mkvmuxer::Tracks::kVp8CodecId; break;
    case VP9_CODEC_ID: codec_id_str = mkvmuxer::Tracks::kVp9CodecId; break;
    case AV1_CODEC_ID: codec_id_str = mkvmuxer::Tracks::kAv1CodecId; break;
    default: return ResultCode::BadParam;
    }

    TrackNum track_num = segment->AddVideoTrack(width, height, number);
    if(track_num == 0) { return ResultCode::UnknownLibwebmError; }

    auto video = static_cast<MuxVideoTrackPtr>(segment->GetTrackByNumber(track_num));
    video->set_codec_id(codec_id_str);

    *track_num_out = track_num;
    return ResultCode::Ok;
  }
  ResultCode mux_segment_add_audio_track(MuxSegmentPtr segment, const int32_t sample_rate,
                                               const int32_t channels, const int32_t number,
                                               const uint32_t codec_id, TrackNum* track_num_out) {
    if(segment == nullptr || track_num_out == nullptr) { return ResultCode::BadParam; }

    const char* codec_id_str = nullptr;
    switch(codec_id) {
    case OPUS_CODEC_ID: codec_id_str = mkvmuxer::Tracks::kOpusCodecId; break;
    case VORBIS_CODEC_ID: codec_id_str = mkvmuxer::Tracks::kVorbisCodecId; break;
    default: return ResultCode::BadParam;
    }

    const auto track_num = segment->AddAudioTrack(sample_rate, channels, number);
    if(track_num == 0) { return ResultCode::UnknownLibwebmError; }

    auto audio = static_cast<MuxAudioTrackPtr>(segment->GetTrackByNumber(track_num));
    audio->set_codec_id(codec_id_str);

    *track_num_out = track_num;
    return ResultCode::Ok;
  }

  ResultCode mux_set_color(MuxSegmentPtr segment, TrackNum video_track_num, uint8_t bits, uint8_t sampling_horiz, uint8_t sampling_vert, uint8_t color_range) {
    mkvmuxer::Colour color;

    MuxTrackPtr track = segment->GetTrackByNumber(video_track_num);
    if(track == nullptr || track->type() != mkvmuxer::Tracks::kVideo) { return ResultCode::BadParam; }
    auto video = static_cast<MuxVideoTrackPtr>(track);

    color.set_bits_per_channel(bits);
    color.set_chroma_subsampling_horz(sampling_horiz);
    color.set_chroma_subsampling_vert(sampling_vert);

    color.set_range(color_range);
    bool success = video->SetColour(color);

    return success ? ResultCode::Ok : ResultCode::UnknownLibwebmError;
  }

  ResultCode mux_segment_add_frame(MuxSegmentPtr segment, TrackNum track_num,
                             const uint8_t* frame, const size_t length,
                             const uint64_t timestamp_ns, const bool keyframe) {
    if(segment == nullptr) { return ResultCode::BadParam; }

    bool success = segment->AddFrame(frame, length, track_num, timestamp_ns, keyframe);
    return success ? ResultCode::Ok : ResultCode::UnknownLibwebmError;
  }

}
