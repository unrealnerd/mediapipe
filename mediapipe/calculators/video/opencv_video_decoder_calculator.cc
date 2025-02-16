// Copyright 2019 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/formats/image_format.pb.h"
#include "mediapipe/framework/formats/image_frame.h"
#include "mediapipe/framework/formats/image_frame_opencv.h"
#include "mediapipe/framework/formats/video_stream_header.h"
#include "mediapipe/framework/port/opencv_imgproc_inc.h"
#include "mediapipe/framework/port/opencv_video_inc.h"
#include "mediapipe/framework/port/status.h"
#include "mediapipe/framework/tool/status_util.h"

namespace mediapipe {

namespace {
// cv::VideoCapture set data type to unsigned char by default. Therefore, the
// image format is only related to the number of channles the cv::Mat has.
ImageFormat::Format GetImageFormat(int num_channels) {
  ImageFormat::Format format;
  switch (num_channels) {
    case 1:
      format = ImageFormat::GRAY8;
      break;
    case 3:
      format = ImageFormat::SRGB;
      break;
    case 4:
      format = ImageFormat::SRGBA;
      break;
    default:
      format = ImageFormat::UNKNOWN;
      break;
  }
  return format;
}
}  // namespace

// This Calculator takes no input streams and produces video packets.
// All streams and input side packets are specified using tags and all of them
// are optional.
//
// Output Streams:
//   VIDEO: Output video frames (ImageFrame).
//   VIDEO_PRESTREAM:
//       Optional video header information output at
//       Timestamp::PreStream() for the corresponding stream.
// Input Side Packets:
//   INPUT_FILE_PATH: The input file path.
//
// Example config:
// node {
//   calculator: "OpenCvVideoDecoderCalculator"
//   input_side_packet: "INPUT_FILE_PATH:input_file_path"
//   output_stream: "VIDEO:video_frames"
//   output_stream: "VIDEO_PRESTREAM:video_header"
// }
class OpenCvVideoDecoderCalculator : public CalculatorBase {
 public:
  static ::mediapipe::Status GetContract(CalculatorContract* cc) {
    cc->InputSidePackets().Tag("INPUT_FILE_PATH").Set<std::string>();
    cc->Outputs().Tag("VIDEO").Set<ImageFrame>();
    if (cc->Outputs().HasTag("VIDEO_PRESTREAM")) {
      cc->Outputs().Tag("VIDEO_PRESTREAM").Set<VideoHeader>();
    }
    return ::mediapipe::OkStatus();
  }

  ::mediapipe::Status Open(CalculatorContext* cc) override {
    const std::string& input_file_path =
        cc->InputSidePackets().Tag("INPUT_FILE_PATH").Get<std::string>();
    cap_ = absl::make_unique<cv::VideoCapture>(input_file_path);
    if (!cap_->isOpened()) {
      return ::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
             << "Fail to open video file at " << input_file_path;
    }
    width_ = static_cast<int>(cap_->get(cv::CAP_PROP_FRAME_WIDTH));
    height_ = static_cast<int>(cap_->get(cv::CAP_PROP_FRAME_HEIGHT));
    double fps = static_cast<double>(cap_->get(cv::CAP_PROP_FPS));
    frame_count_ = static_cast<int>(cap_->get(cv::CAP_PROP_FRAME_COUNT));
    // Unfortunately, cap_->get(cv::CAP_PROP_FORMAT) always returns CV_8UC1
    // back. To get correct image format, we read the first frame from the video
    // and get the number of channels.
    cv::Mat frame;
    cap_->read(frame);
    if (frame.empty()) {
      return ::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
             << "Fail to read any frames from the video file at "
             << input_file_path;
    }
    format_ = GetImageFormat(frame.channels());
    if (format_ == ImageFormat::UNKNOWN) {
      return ::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
             << "Unsupported video format of the video file at "
             << input_file_path;
    }

    if (fps <= 0 || frame_count_ <= 0 || width_ <= 0 || height_ <= 0) {
      return ::mediapipe::InvalidArgumentErrorBuilder(MEDIAPIPE_LOC)
             << "Fail to make video header due to the incorrect metadata from "
                "the video file at "
             << input_file_path;
    }
    auto header = absl::make_unique<VideoHeader>();
    header->format = format_;
    header->width = width_;
    header->height = height_;
    header->frame_rate = fps;
    header->duration = frame_count_ / fps;

    if (cc->Outputs().HasTag("VIDEO_PRESTREAM")) {
      cc->Outputs()
          .Tag("VIDEO_PRESTREAM")
          .Add(header.release(), Timestamp::PreStream());
      cc->Outputs().Tag("VIDEO_PRESTREAM").Close();
    }
    // Rewind to the very first frame.
    cap_->set(cv::CAP_PROP_POS_AVI_RATIO, 0);
    return ::mediapipe::OkStatus();
  }

  ::mediapipe::Status Process(CalculatorContext* cc) override {
    auto image_frame = absl::make_unique<ImageFrame>(format_, width_, height_,
                                                     /*alignment_boundary=*/1);
    // Use microsecond as the unit of time.
    Timestamp timestamp(cap_->get(cv::CAP_PROP_POS_MSEC) * 1000);
    if (format_ == ImageFormat::GRAY8) {
      cv::Mat frame = formats::MatView(image_frame.get());
      cap_->read(frame);
      if (frame.empty()) {
        return tool::StatusStop();
      }
    } else {
      cv::Mat tmp_frame;
      cap_->read(tmp_frame);
      if (tmp_frame.empty()) {
        return tool::StatusStop();
      }
      if (format_ == ImageFormat::SRGB) {
        cv::cvtColor(tmp_frame, formats::MatView(image_frame.get()),
                     cv::COLOR_BGR2RGB);
      } else if (format_ == ImageFormat::SRGBA) {
        cv::cvtColor(tmp_frame, formats::MatView(image_frame.get()),
                     cv::COLOR_BGRA2RGBA);
      }
    }
    // If the timestamp of the current frame is not greater than the one of the
    // previous frame, the new frame will be discarded.
    if (prev_timestamp_ < timestamp) {
      cc->Outputs().Tag("VIDEO").Add(image_frame.release(), timestamp);
      prev_timestamp_ = timestamp;
      decoded_frames_++;
    }

    return ::mediapipe::OkStatus();
  }

  ::mediapipe::Status Close(CalculatorContext* cc) override {
    if (cap_ && cap_->isOpened()) {
      cap_->release();
    }
    if (decoded_frames_ != frame_count_) {
      LOG(WARNING) << "Not all the frames are decoded (total frames: "
                   << frame_count_ << " vs decoded frames: " << decoded_frames_
                   << ").";
    }
    return ::mediapipe::OkStatus();
  }

 private:
  std::unique_ptr<cv::VideoCapture> cap_;
  int width_;
  int height_;
  int frame_count_;
  int decoded_frames_ = 0;
  ImageFormat::Format format_;
  Timestamp prev_timestamp_ = Timestamp::Unset();
};

REGISTER_CALCULATOR(OpenCvVideoDecoderCalculator);
}  // namespace mediapipe
