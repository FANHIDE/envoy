#pragma once

#include <cstdint>
#include <queue>
#include <string>

#include "envoy/http/codec.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/c_smart_ptr.h"
#include "common/common/logger.h"

#include "nghttp2/nghttp2.h"

namespace Envoy {
namespace Http {
namespace Http2 {

/**
 * A class that creates and sends METADATA payload. The METADATA payload is a group of string key
 * value pairs encoded in HTTP/2 header blocks.
 */
class MetadataEncoder : Logger::Loggable<Logger::Id::http2> {
public:
  MetadataEncoder();

  /**
   * Creates wire format HTTP/2 header block from a vector of metadata maps.
   * @param metadata_map_vector supplies the metadata map vector to encode.
   * @return whether encoding is successful.
   */
  bool createPayload(const MetadataMapVector& metadata_map_vector);

  /**
   * @return true if there is payload to be submitted.
   */
  bool hasNextFrame();

  /**
   * Creates the metadata frame payload for the next metadata frame.
   * @param buf is the pointer to the destination memory where the payload should be copied to. len
   * is the largest length the memory can hold.
   * @return the size of frame payload.
   */
  uint64_t packNextFramePayload(uint8_t* buf, const size_t len);

  /**
   * Returns end_metadata value for the next metadata frame.
   * @return end_metadata value.
   */
  uint8_t nextEndMetadata();

  /**
   * Estimates upper bound of the number of frames the payload_ can generate.
   * @return frame count upper bound.
   */
  uint64_t frameCountUpperBound();

private:
  /**
   * Creates wire format HTTP/2 header block from metadata_map.
   * @param metadata_map supplies METADATA to encode.
   * @return whether encoding is successful.
   */
  bool createPayloadMetadataMap(const MetadataMap& metadata_map);

  /**
   * Creates wired format header blocks using nghttp2.
   * @param metadata_map supplies METADATA to encode.
   * @return true if the creation succeeds.
   */
  bool createHeaderBlockUsingNghttp2(const MetadataMap& metadata_map);

  // The METADATA payload to be sent.
  Buffer::OwnedImpl payload_;

  // Max payload size bound.
  const uint64_t max_payload_size_bound_ = 1024 * 1024;

  // Default HPACK table size.
  const size_t header_table_size_ = 4096;

  // TODO(soya3129): share deflater among all encoders in the same connection. The benefit is less
  // memory, and the caveat is encoding error on one stream can impact other streams.
  typedef CSmartPtr<nghttp2_hd_deflater, nghttp2_hd_deflate_del> Deflater;
  Deflater deflater_;

  // Stores the remaining payload size of each metadata_map to be packed. The payload size is needed
  // so that we know when END_METADATA should be set. The payload size gets updated when the payload
  // is packed into metadata frames.
  std::queue<uint64_t> payload_size_queue_;
};

} // namespace Http2
} // namespace Http
} // namespace Envoy
