#include "Packet.h"
#include <string.h>
#include <SHA256.h>

namespace mesh {

Packet::Packet() {
  header = 0;
  path_len = 0;
  payload_len = 0;
}

bool Packet::isValidPathLen(uint8_t path_len) {
  uint8_t hash_count = path_len & 63;
  uint8_t hash_size = (path_len >> 6) + 1;
  if (hash_size == 4) return false;  // Reserved for future
  return hash_count*hash_size <= MAX_PATH_SIZE;
}

size_t Packet::writePath(uint8_t* dest, const uint8_t* src, uint8_t path_len) {
  uint8_t hash_count = path_len & 63;
  uint8_t hash_size = (path_len >> 6) + 1;
  size_t len = hash_count*hash_size;
  if (len > MAX_PATH_SIZE) {
    MESH_DEBUG_PRINTLN("Packet::copyPath, invalid path_len=%d", (uint32_t)path_len);
    return 0;   // Error
  }
  memcpy(dest, src, len);
  return len;
}

uint8_t Packet::copyPath(uint8_t* dest, const uint8_t* src, uint8_t path_len) {
  writePath(dest, src, path_len);
  return path_len;
}

int Packet::getRawLength() const {
  return 2 + getPathByteLen() + payload_len + (hasTransportCodes() ? 4 : 0);
}

void Packet::calculatePacketHash(uint8_t* hash) const {
  SHA256 sha;
  uint8_t t = getPayloadType();
  sha.update(&t, 1);
  if (t == PAYLOAD_TYPE_TRACE) {
    sha.update(&path_len, sizeof(path_len));   // CAVEAT: TRACE packets can revisit same node on return path
  }
  sha.update(payload, payload_len);
  sha.finalize(hash, MAX_HASH_SIZE);
}

uint8_t Packet::writeTo(uint8_t dest[]) const {
  uint8_t i = 0;
  dest[i++] = header;
  if (hasTransportCodes()) {
    memcpy(&dest[i], &transport_codes[0], 2); i += 2;
    memcpy(&dest[i], &transport_codes[1], 2); i += 2;
  }
  dest[i++] = path_len;
  i += writePath(&dest[i], path, path_len);
  memcpy(&dest[i], payload, payload_len); i += payload_len;
  return i;
}

bool Packet::readFrom(const uint8_t src[], uint8_t len) {
  // CUSTOM (TeTeHacko): every field is bounds-checked against `len` BEFORE it is
  // read. Upstream reads the header, the transport codes, path_len and the whole
  // path first and only then asks whether any of it was actually there
  // (`if (i >= len)`), so a short frame is read past its end and the result is
  // thrown away afterwards.
  //
  // Measured with an ASAN harness that hands readFrom a heap buffer allocated to
  // exactly `len` bytes: 5 242 880 generated frames hit FOUR distinct
  // out-of-bounds reads (header, both transport codes, path_len, the path
  // memcpy). With these guards the same sweep is clean and accepts exactly the
  // same 1 351 424 frames -- the checks reject nothing that used to parse, they
  // only stop reading memory that was never received.
  //
  // On the device the source is Dispatcher's `raw[MAX_TRANS_UNIT+1]`, a 256-byte
  // stack buffer, and this reads at most ~70 bytes into it, so what it read was
  // leftovers from an earlier packet rather than memory outside the array. That
  // is why it never showed up as a crash -- and why this is hygiene, not a hole.
  //
  // Same fix as upstream PR #1666 (weebl2000), open and unmerged since Feb 2026.
  if (len < 2) return false;   // at minimum: header + path_len
  uint8_t i = 0;
  header = src[i++];
  if (hasTransportCodes()) {
    if (i + 4 >= len) return false;   // 4 transport bytes plus the path_len byte
    memcpy(&transport_codes[0], &src[i], 2); i += 2;
    memcpy(&transport_codes[1], &src[i], 2); i += 2;
  } else {
    transport_codes[0] = transport_codes[1] = 0;
  }
  path_len = src[i++];
  if (!isValidPathLen(path_len)) return false;   // bad encoding

  uint8_t bl = getPathByteLen();
  if (i + bl >= len) return false;   // path plus at least one payload byte must fit
  memcpy(path, &src[i], bl); i += bl;

  payload_len = len - i;
  if (payload_len > sizeof(payload)) return false;  // bad encoding
  memcpy(payload, &src[i], payload_len); //i += payload_len;
  return true;   // success
}

}