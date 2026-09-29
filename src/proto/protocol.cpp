#include "protocol.hpp"

#include <cstring>
#include <stdexcept>

namespace kv {
namespace {

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& buffer, std::size_t offset) {
    if (offset + 4 > buffer.size()) {
        throw std::runtime_error("truncated frame header");
    }
    return static_cast<std::uint32_t>(buffer[offset]) |
           (static_cast<std::uint32_t>(buffer[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(buffer[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(buffer[offset + 3]) << 24);
}

void write_u32_le(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
}

std::vector<std::uint8_t> make_response(std::uint8_t type, const std::string& payload) {
    std::vector<std::uint8_t> out;
    const std::uint32_t total = static_cast<std::uint32_t>(1 + payload.size());
    write_u32_le(out, total);
    out.push_back(type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

}  // namespace

std::optional<Request> FrameCodec::decode_one(const std::vector<std::uint8_t>& buffer, std::size_t& offset) {
    const std::size_t start = offset;
    if (buffer.size() - offset < 4) {
        return std::nullopt;
    }

    std::size_t pos = offset;
    const auto nargs = static_cast<std::size_t>(read_u32_le(buffer, pos));
    pos += 4;

    Request request;
    request.argv.reserve(nargs);
    for (std::size_t i = 0; i < nargs; ++i) {
        if (buffer.size() - pos < 4) {
            return std::nullopt;
        }
        const auto len = read_u32_le(buffer, pos);
        pos += 4;
        if (buffer.size() - pos < len) {
            return std::nullopt;
        }
        request.argv.emplace_back(reinterpret_cast<const char*>(buffer.data() + pos), len);
        pos += len;
    }

    offset = pos;
    return request;
}

std::vector<std::uint8_t> FrameCodec::encode_request(const std::vector<std::string>& argv) {
    std::vector<std::uint8_t> out;
    const std::uint32_t nargs = static_cast<std::uint32_t>(argv.size());
    write_u32_le(out, nargs);
    for (const auto& arg : argv) {
        const std::uint32_t len = static_cast<std::uint32_t>(arg.size());
        write_u32_le(out, len);
        out.insert(out.end(), arg.begin(), arg.end());
    }
    return out;
}

std::vector<std::uint8_t> FrameCodec::encode_response_ok() {
    return make_response(2, std::string("OK"));
}

std::vector<std::uint8_t> FrameCodec::encode_response_error(const std::string& message) {
    return make_response(1, message);
}

std::vector<std::uint8_t> FrameCodec::encode_response_string(const std::string& value) {
    return make_response(2, value);
}

std::vector<std::uint8_t> FrameCodec::encode_response_int(std::int64_t value) {
    return make_response(3, std::to_string(value));
}

std::vector<std::uint8_t> FrameCodec::encode_response_nil() {
    return make_response(0, std::string());
}

std::vector<std::uint8_t> FrameCodec::encode_response_array(const std::vector<std::string>& values) {
    std::string payload;
    payload.reserve(64);
    for (const auto& v : values) {
        if (!payload.empty()) {
            payload.push_back('\n');
        }
        payload += v;
    }
    return make_response(4, payload);
}

}  // namespace kv
