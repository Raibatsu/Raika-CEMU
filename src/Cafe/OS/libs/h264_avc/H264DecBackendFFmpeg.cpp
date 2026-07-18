#include "H264DecInternal.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <thread>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace H264
{
	bool H264_IsBotW();

	class H264FFmpegDecoder final : public H264DecoderBackend
	{
	  public:
		static H264FFmpegDecoder* Create()
		{
			auto* decoder = new (std::nothrow) H264FFmpegDecoder();
			if (!decoder || !decoder->Open())
			{
				delete decoder;
				return nullptr;
			}
			decoder->m_decoderThread = std::thread(&H264FFmpegDecoder::DecoderThread, decoder);
			return decoder;
		}

		~H264FFmpegDecoder() override
		{
			m_threadShouldExit.store(true, std::memory_order_release);
			m_decodeSem.increment();
			if (m_decoderThread.joinable())
				m_decoderThread.join();
			Destroy();
		}

		void Init(bool isBufferedMode) override
		{
			(void)isBufferedMode;
			if (m_codecContext)
				avcodec_flush_buffers(m_codecContext);
		}

		void Destroy() override
		{
			if (m_swsContext)
			{
				sws_freeContext(m_swsContext);
				m_swsContext = nullptr;
			}
			avcodec_free_context(&m_codecContext);
			av_buffer_unref(&m_deviceContext);
		}

	  private:
		static AVPixelFormat SelectFormat(AVCodecContext*, const AVPixelFormat* formats)
		{
			for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
				if (*format == AV_PIX_FMT_NVTEGRA)
					return *format;
			return AV_PIX_FMT_NONE;
		}

		bool Open()
		{
			av_log_set_level(AV_LOG_QUIET);
			if (av_hwdevice_ctx_create(&m_deviceContext, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0) < 0)
				return false;

			const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
			if (!codec)
				return false;
			m_codecContext = avcodec_alloc_context3(codec);
			if (!m_codecContext)
				return false;

			m_codecContext->hw_device_ctx = av_buffer_ref(m_deviceContext);
			if (!m_codecContext->hw_device_ctx)
				return false;
			m_codecContext->get_format = SelectFormat;
			m_codecContext->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
			m_codecContext->thread_count = 1;
			m_codecContext->extra_hw_frames = 2;
			return avcodec_open2(m_codecContext, codec, nullptr) >= 0;
		}

		void QueueResult(uint32 index, bool hasFrame, const AVFrame* frame = nullptr)
		{
			std::unique_lock lock(m_decodeQueueMtx);
			if (index >= m_decodedSliceArray.size())
				return;
			auto& slice = m_decodedSliceArray[index];
			if (!slice.isUsed || slice.result.isDecoded)
				return;

			slice.result.isDecoded = true;
			slice.result.hasFrame = hasFrame;
			if (frame)
			{
				slice.result.frameWidth = frame->width;
				slice.result.frameHeight = H264_IsBotW() && frame->width == 1920 && frame->height == 1088 ? 1080 : frame->height;
				slice.result.bytesPerRow = (frame->width + 0xFF) & ~0xFF;
				slice.result.cropTop = static_cast<sint32>(frame->crop_top);
				slice.result.cropBottom = static_cast<sint32>(frame->crop_bottom);
				slice.result.cropLeft = static_cast<sint32>(frame->crop_left);
				slice.result.cropRight = static_cast<sint32>(frame->crop_right);
				slice.result.cropEnable = frame->crop_top || frame->crop_bottom || frame->crop_left || frame->crop_right;
			}
			m_displayQueue.push_back(index);
			lock.unlock();
			coreinit::OSSignalEvent(m_displayQueueEvt);
		}

		uint32 FrameIndex(const AVFrame* frame, uint32 fallback) const
		{
			const uintptr_t opaque = reinterpret_cast<uintptr_t>(frame->opaque);
			if (opaque > 0 && opaque <= m_decodedSliceArray.size())
				return static_cast<uint32>(opaque - 1);
			if (frame->pts >= 0 && frame->pts < static_cast<int64_t>(m_decodedSliceArray.size()))
				return static_cast<uint32>(frame->pts);
			return fallback;
		}

		bool CopyFrame(AVFrame* frame, uint32 index)
		{
			AVFrame* source = frame;
			AVFrame* transferred = nullptr;
			if (frame->format == AV_PIX_FMT_NVTEGRA)
			{
				transferred = av_frame_alloc();
				if (!transferred || av_hwframe_transfer_data(transferred, frame, 0) < 0)
				{
					av_frame_free(&transferred);
					return false;
				}
				source = transferred;
			}

			if (index >= m_decodedSliceArray.size() || !m_decodedSliceArray[index].result.imageOutput)
			{
				av_frame_free(&transferred);
				return false;
			}

			const int width = frame->width;
			const int height = H264_IsBotW() && width == 1920 && frame->height == 1088 ? 1080 : frame->height;
			const int stride = (width + 0xFF) & ~0xFF;
			auto* output = static_cast<uint8*>(m_decodedSliceArray[index].result.imageOutput);
			uint8* destination[] = {output, output + static_cast<size_t>(stride) * height, nullptr, nullptr};
			int destinationStride[] = {stride, stride, 0, 0};

			m_swsContext = sws_getCachedContext(m_swsContext,
				width, height, static_cast<AVPixelFormat>(source->format),
				width, height, AV_PIX_FMT_NV12, SWS_POINT, nullptr, nullptr, nullptr);
			const bool copied = m_swsContext &&
				sws_scale(m_swsContext, source->data, source->linesize, 0, height,
					destination, destinationStride) == height;
			av_frame_free(&transferred);
			return copied;
		}

		bool DrainFrames(uint32 fallbackIndex)
		{
			AVFrame* frame = av_frame_alloc();
			if (!frame)
				return false;
			bool success = true;
			for (;;)
			{
				const int result = avcodec_receive_frame(m_codecContext, frame);
				if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
					break;
				if (result < 0)
				{
					success = false;
					break;
				}
				const uint32 index = FrameIndex(frame, fallbackIndex);
				if (CopyFrame(frame, index))
					QueueResult(index, true, frame);
				else
					QueueResult(index, false);
				av_frame_unref(frame);
			}
			av_frame_free(&frame);
			return success;
		}

		void Decode(uint32 index)
		{
			if (!m_codecContext || index >= m_decodedSliceArray.size())
			{
				QueueResult(index, false);
				return;
			}

			auto& input = m_decodedSliceArray[index].dataToDecode;
			AVPacket* packet = av_packet_alloc();
			if (!packet || av_new_packet(packet, static_cast<int>(input.m_length)) < 0)
			{
				av_packet_free(&packet);
				QueueResult(index, false);
				return;
			}
			std::memcpy(packet->data, input.m_data, input.m_length);
			packet->pts = index;
			packet->dts = index;
			packet->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(index + 1));

			int result = avcodec_send_packet(m_codecContext, packet);
			if (result == AVERROR(EAGAIN))
			{
				DrainFrames(index);
				result = avcodec_send_packet(m_codecContext, packet);
			}
			av_packet_free(&packet);
			if (result < 0 || !DrainFrames(index))
				QueueResult(index, false);
		}

		void Flush()
		{
			if (m_codecContext)
			{
				avcodec_send_packet(m_codecContext, nullptr);
				DrainFrames(0);
				avcodec_flush_buffers(m_codecContext);
			}
			for (uint32 index = 0; index < m_decodedSliceArray.size(); ++index)
				if (m_decodedSliceArray[index].isUsed && !m_decodedSliceArray[index].result.isDecoded)
					QueueResult(index, false);
		}

		void DecoderThread()
		{
			while (!m_threadShouldExit.load(std::memory_order_acquire))
			{
				m_decodeSem.decrementWithWait();
				if (m_threadShouldExit.load(std::memory_order_acquire))
					break;
				std::unique_lock lock(m_decodeQueueMtx);
				if (m_decodeQueue.empty())
					continue;
				const uint32 index = m_decodeQueue.front();
				m_decodeQueue.erase(m_decodeQueue.begin());
				lock.unlock();
				if (index == CMD_FLUSH)
				{
					Flush();
					coreinit::OSSignalEvent(m_flushEvt);
				}
				else
				{
					Decode(index);
				}
			}
		}

		AVBufferRef* m_deviceContext{};
		AVCodecContext* m_codecContext{};
		SwsContext* m_swsContext{};
		std::thread m_decoderThread;
		std::atomic_bool m_threadShouldExit{false};
	};

	H264DecoderBackend* CreateFFmpegDecoder()
	{
		return H264FFmpegDecoder::Create();
	}
}
