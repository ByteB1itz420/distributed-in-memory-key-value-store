#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kv {

struct Request {
    std::vector<std::string> argv;
};

class FrameCodec {
public:
    static std::optional<Request> decode_one(const std::vector<std::uint8_t>& buffer, std::size_t& offset);
    static std::vector<std::uint8_t> encode_request(const std::vector<std::string>& argv);
    static std::vector<std::uint8_t> encode_response_ok();
    static std::vector<std::uint8_t> encode_response_error(const std::string& message);
    static std::vector<std::uint8_t> encode_response_string(const std::string& value);
    static std::vector<std::uint8_t> encode_response_int(std::int64_t value);
    static std::vector<std::uint8_t> encode_response_nil();
    static std::vector<std::uint8_t> encode_response_array(const std::vector<std::string>& values);
};

}  // namespace kv
