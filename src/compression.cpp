#include "archive_internal.h"

#include <zlib.h>
#include <zstd.h>
#include <zstd_errors.h>

#include <limits>

namespace huxerui::archive {
namespace {

using namespace detail;

enum class Direction { Compress, Decompress };

struct Step {
  std::size_t consumed;
  std::size_t produced;
  bool frame_complete;
};

class Codec {
public:
  Codec(CompressionFormat format, Direction direction) : format_(format), direction_(direction) {}

  ~Codec() {
    if (!gzip_initialized_)
      return;
    if (direction_ == Direction::Compress) {
      deflateEnd(&gzip_);
    } else {
      inflateEnd(&gzip_);
    }
  }

  ArchiveResult<void> Initialize() {
    switch (format_) {
    case CompressionFormat::Gzip: {
      constexpr int gzip_window_bits = MAX_WBITS + 16;
      const int result = direction_ == Direction::Compress ? deflateInit2(&gzip_, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                                                          gzip_window_bits, 8, Z_DEFAULT_STRATEGY)
                                                           : inflateInit2(&gzip_, gzip_window_bits);
      gzip_initialized_ = result == Z_OK;
      if (!gzip_initialized_)
        return ArchiveResult<void>::Failure(GzipFailure(result));
      break;
    }
    case CompressionFormat::Zstd:
      if (direction_ == Direction::Compress) {
        zstd_encoder_.reset(ZSTD_createCCtx());
        if (!zstd_encoder_)
          return ArchiveResult<void>::Failure(AllocationFailed());
        const auto status = ZSTD_CCtx_setParameter(zstd_encoder_.get(), ZSTD_c_checksumFlag, 1);
        if (ZSTD_isError(status))
          return ArchiveResult<void>::Failure(ZstdFailure(status));
      } else {
        zstd_decoder_.reset(ZSTD_createDCtx());
        if (!zstd_decoder_)
          return ArchiveResult<void>::Failure(AllocationFailed());
        constexpr int max_window_log = 27;
        const auto status = ZSTD_DCtx_setParameter(zstd_decoder_.get(), ZSTD_d_windowLogMax, max_window_log);
        if (ZSTD_isError(status))
          return ArchiveResult<void>::Failure(ZstdFailure(status));
      }
      break;
    default:
      return ArchiveResult<void>::Failure(Error(ArchiveErrorCode::InvalidArgument, "Invalid compression format"));
    }
    return ArchiveResult<void>::Success();
  }

  ArchiveResult<Step> Transform(std::span<const std::byte> input, std::span<std::byte> output, bool eof) {
    if (format_ == CompressionFormat::Gzip) {
      return TransformGzip(input, output, eof);
    }
    ZSTD_inBuffer source{input.data(), input.size(), 0};
    ZSTD_outBuffer destination{output.data(), output.size(), 0};
    const auto status =
        direction_ == Direction::Compress
            ? ZSTD_compressStream2(zstd_encoder_.get(), &destination, &source, eof ? ZSTD_e_end : ZSTD_e_continue)
            : ZSTD_decompressStream(zstd_decoder_.get(), &destination, &source);
    if (ZSTD_isError(status))
      return ArchiveResult<Step>::Failure(ZstdFailure(status));
    return ArchiveResult<Step>::Success(
        {source.pos, destination.pos, status == 0 && (direction_ == Direction::Decompress || eof)});
  }

private:
  ArchiveError AllocationFailed() const {
    return Error(ArchiveErrorCode::Io, "Could not allocate a compression context");
  }

  ArchiveError GzipFailure(int status) const {
    if (status == Z_MEM_ERROR)
      return AllocationFailed();
    return Error(direction_ == Direction::Decompress ? ArchiveErrorCode::InvalidData : ArchiveErrorCode::Io,
                 gzip_.msg ? gzip_.msg : "gzip operation failed");
  }

  ArchiveError ZstdFailure(std::size_t status) const {
    const auto code = ZSTD_getErrorCode(status);
    if (code == ZSTD_error_frameParameter_windowTooLarge) {
      return Error(ArchiveErrorCode::LimitExceeded, "Zstandard decoder window exceeds the supported limit");
    }
    if (code == ZSTD_error_memory_allocation)
      return AllocationFailed();
    return Error(direction_ == Direction::Decompress ? ArchiveErrorCode::InvalidData : ArchiveErrorCode::Io,
                 ZSTD_getErrorName(status));
  }

  ArchiveResult<Step> TransformGzip(std::span<const std::byte> input, std::span<std::byte> output, bool eof) {
    // Each gzip member has a separate trailer and checksum.
    if (gzip_member_complete_) {
      const int status = inflateReset(&gzip_);
      if (status != Z_OK)
        return ArchiveResult<Step>::Failure(GzipFailure(status));
    }
    gzip_.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    gzip_.avail_in = static_cast<uInt>(input.size());
    gzip_.next_out = reinterpret_cast<Bytef*>(output.data());
    gzip_.avail_out = static_cast<uInt>(output.size());
    const int status =
        direction_ == Direction::Compress ? deflate(&gzip_, eof ? Z_FINISH : Z_NO_FLUSH) : inflate(&gzip_, Z_NO_FLUSH);
    if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
      return ArchiveResult<Step>::Failure(GzipFailure(status));
    }
    gzip_member_complete_ = direction_ == Direction::Decompress && status == Z_STREAM_END;
    return ArchiveResult<Step>::Success(
        {input.size() - gzip_.avail_in, output.size() - gzip_.avail_out, status == Z_STREAM_END});
  }

  CompressionFormat format_;
  Direction direction_;
  z_stream gzip_{};
  bool gzip_initialized_ = false;
  bool gzip_member_complete_ = false;
  std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> zstd_encoder_{nullptr, ZSTD_freeCCtx};
  std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> zstd_decoder_{nullptr, ZSTD_freeDCtx};
};

class Transfer {
public:
  Transfer(CompressionFormat format, Direction direction, std::uint64_t limit)
      : codec(format, direction), direction_(direction), limit_(limit) {}

  ArchiveResult<std::size_t> Transform(std::span<const std::byte>& input, std::span<std::byte> output, bool eof) {
    if (eof && frame_complete_) {
      done = true;
      return ArchiveResult<std::size_t>::Success(0);
    }
    auto transformed = codec.Transform(input, output, eof);
    if (!transformed.Succeeded())
      return ArchiveResult<std::size_t>::Failure(std::move(transformed).Error());

    const auto& step = transformed.Value();
    input = input.subspan(step.consumed);
    frame_complete_ = step.frame_complete;
    done = eof && frame_complete_;
    needs_input = !eof && input.empty() && (frame_complete_ || step.produced < output.size());
    if (!done && !needs_input && step.consumed == 0 && step.produced == 0) {
      return ArchiveResult<std::size_t>::Failure(
          Error(direction_ == Direction::Decompress ? ArchiveErrorCode::InvalidData : ArchiveErrorCode::Io,
                direction_ == Direction::Decompress ? "Compressed stream ended before its trailer"
                                                    : "Encoder made no progress"));
    }
    if (step.produced > limit_ - written) {
      return ArchiveResult<std::size_t>::Failure(
          Error(ArchiveErrorCode::LimitExceeded, "Compression output limit exceeded"));
    }
    written += step.produced;
    return ArchiveResult<std::size_t>::Success(step.produced);
  }

  Codec codec;
  bool needs_input = true;
  bool done = false;
  std::uint64_t written = 0;

private:
  Direction direction_;
  std::uint64_t limit_;
  bool frame_complete_ = false;
};

using ByteCount = ArchiveResult<std::uint64_t>;
constexpr auto Unlimited = std::numeric_limits<std::uint64_t>::max();

ByteCount Run(InputStream input, OutputStream output, CompressionFormat format, Direction direction,
              std::uint64_t limit) {
  Transfer transfer(format, direction, limit);
  auto initialized = transfer.codec.Initialize();
  if (!initialized.Succeeded())
    return ByteCount::Failure(std::move(initialized).Error());

  Bytes incoming(ChunkSize);
  Bytes outgoing(ChunkSize);
  std::span<const std::byte> pending;
  bool eof = false;
  while (!transfer.done) {
    if (transfer.needs_input) {
      auto result = input.Read(incoming);
      if (!result.Succeeded())
        return ByteCount::Failure(IoFailure(std::move(result).Error()));
      pending = std::span(incoming).first(result.Value());
      eof = pending.empty();
    }
    auto result = transfer.Transform(pending, outgoing, eof);
    if (!result.Succeeded())
      return ByteCount::Failure(std::move(result).Error());
    if (result.Value() != 0) {
      auto written = output.Write(std::span(outgoing).first(result.Value()));
      if (!written.Succeeded())
        return ByteCount::Failure(IoFailure(std::move(written).Error()));
    }
  }
  auto closed = output.Close();
  if (!closed.Succeeded())
    return ByteCount::Failure(IoFailure(std::move(closed).Error()));
  return ByteCount::Success(transfer.written);
}

Task<ByteCount> RunAsync(AsyncInputStream input, AsyncOutputStream output, CompressionFormat format,
                         Direction direction, std::uint64_t limit) {
  Transfer transfer(format, direction, limit);
  auto initialized = transfer.codec.Initialize();
  if (!initialized.Succeeded())
    co_return ByteCount::Failure(std::move(initialized).Error());

  Bytes incoming;
  std::span<const std::byte> pending;
  bool eof = false;
  while (!transfer.done) {
    if (transfer.needs_input) {
      auto result = co_await input.ReadAsync(ChunkSize);
      if (!result.Succeeded())
        co_return ByteCount::Failure(IoFailure(std::move(result).Error()));
      incoming = std::move(result).Value();
      pending = incoming;
      eof = pending.empty();
    }
    Bytes outgoing(ChunkSize);
    auto result = transfer.Transform(pending, outgoing, eof);
    if (!result.Succeeded())
      co_return ByteCount::Failure(std::move(result).Error());
    if (result.Value() != 0) {
      outgoing.resize(result.Value());
      auto written = co_await output.WriteAsync(std::move(outgoing));
      if (!written.Succeeded())
        co_return ByteCount::Failure(IoFailure(std::move(written).Error()));
    }
  }
  auto closed = co_await output.CloseAsync();
  if (!closed.Succeeded())
    co_return ByteCount::Failure(IoFailure(std::move(closed).Error()));
  co_return ByteCount::Success(transfer.written);
}

} // namespace

ArchiveResult<std::uint64_t> CompressStream(InputStream input, OutputStream output, CompressionFormat format) {
  return Run(std::move(input), std::move(output), format, Direction::Compress, Unlimited);
}

ArchiveResult<std::uint64_t> DecompressStream(InputStream input, OutputStream output, CompressionFormat format,
                                              std::uint64_t max_output_bytes) {
  return Run(std::move(input), std::move(output), format, Direction::Decompress, max_output_bytes);
}

Task<ArchiveResult<std::uint64_t>> CompressStreamAsync(AsyncInputStream input, AsyncOutputStream output,
                                                       CompressionFormat format) {
  return RunAsync(std::move(input), std::move(output), format, Direction::Compress, Unlimited);
}

Task<ArchiveResult<std::uint64_t>> DecompressStreamAsync(AsyncInputStream input, AsyncOutputStream output,
                                                         CompressionFormat format, std::uint64_t max_output_bytes) {
  return RunAsync(std::move(input), std::move(output), format, Direction::Decompress, max_output_bytes);
}

} // namespace huxerui::archive
