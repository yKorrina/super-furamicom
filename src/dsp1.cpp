#include "dsp1.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ostream>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kAngleScale = (2.0 * kPi) / 65536.0;
constexpr double kQ15Scale = 32768.0;
constexpr double kMinDepth = 1.0;
constexpr int16_t kClipVOffsetCurveA = 0x6488;
constexpr int16_t kClipVOffsetCurveB = 0x14AC;
constexpr int16_t kClipCosCurveA = 0x277A;
constexpr int16_t kClipCosCurveB = 0x0A26;
constexpr std::array<int16_t, 16> kMaxZenithByExponent = {
    0x38b4, 0x38b7, 0x38ba, 0x38be,
    0x38c0, 0x38c4, 0x38c7, 0x38ca,
    0x38ce, 0x38d0, 0x38d4, 0x38d7,
    0x38da, 0x38dd, 0x38e0, 0x38e4,
};

int16_t clampS16(long long value) {
    if (value < -32768LL) return -32768;
    if (value > 32767LL) return 32767;
    return static_cast<int16_t>(value);
}

int16_t clampFloatToS16(double value) {
    if (!std::isfinite(value)) return 0;
    if (value < -32768.0) return -32768;
    if (value > 32767.0) return 32767;
    return static_cast<int16_t>(std::llround(value));
}

double angleToRadians(int16_t angle) {
    return static_cast<double>(angle) * kAngleScale;
}

int16_t q15Sin(int16_t angle) {
    if (angle == std::numeric_limits<int16_t>::min()) return 0;
    return clampFloatToS16(std::sin(angleToRadians(angle)) * 32767.0);
}

int16_t q15Cos(int16_t angle) {
    if (angle == std::numeric_limits<int16_t>::min()) return std::numeric_limits<int16_t>::min();
    return clampFloatToS16(std::cos(angleToRadians(angle)) * 32767.0);
}

int16_t mulQ15(int16_t a, int16_t b) {
    return static_cast<int16_t>((static_cast<int32_t>(a) * static_cast<int32_t>(b)) >> 15);
}

double safeDiv(double numerator, double denominator) {
    if (!std::isfinite(numerator)) return 0.0;
    if (!std::isfinite(denominator) || std::fabs(denominator) < 1e-6) {
        return numerator >= 0.0 ? 32767.0 : -32768.0;
    }
    return numerator / denominator;
}

double q15ToUnit(int16_t value) {
    return static_cast<double>(value) / kQ15Scale;
}

int normalizedShift16(int16_t value) {
    int shift = 0;
    int16_t bit = 0x4000;
    if (value < 0) {
        while ((value & bit) && bit) {
            bit >>= 1;
            shift++;
        }
    } else {
        while (!(value & bit) && bit) {
            bit >>= 1;
            shift++;
        }
    }
    return shift;
}

int16_t arithmeticShiftRight(int32_t value, int shift) {
    if (shift <= 0) {
        return clampS16(value);
    }
    if (shift >= 31) {
        return value < 0 ? -1 : 0;
    }
    if (value >= 0) {
        return clampS16(value >> shift);
    }
    return clampS16(-static_cast<int32_t>((((-value) - 1) >> shift) + 1));
}

void normalizeFixed(int32_t value, int16_t& coefficient, int16_t& exponent) {
    if (value == 0) {
        coefficient = 0;
        exponent = 0;
        return;
    }

    int exp = 0;
    while (value < -32768 || value > 32767) {
        value = arithmeticShiftRight(value, 1);
        exp++;
    }

    const int16_t m = static_cast<int16_t>(value);
    const int shift = normalizedShift16(m);
    coefficient = shift > 0 ? clampS16(static_cast<int32_t>(m) << shift) : m;
    exponent = static_cast<int16_t>(exp - shift);
}

void inverseCoefficient(int16_t coefficient, int16_t exponent, int16_t& out_coefficient, int16_t& out_exponent) {
    if (coefficient == 0) {
        out_coefficient = 0x7FFF;
        out_exponent = 0x002F;
        return;
    }

    int sign = 1;
    int32_t magnitude = coefficient;
    int normalized_exponent = exponent;
    if (magnitude < 0) {
        if (magnitude < -32767) {
            magnitude = -32767;
        }
        magnitude = -magnitude;
        sign = -1;
    }

    while (magnitude < 0x4000) {
        magnitude <<= 1;
        normalized_exponent--;
    }

    if (magnitude == 0x4000) {
        if (sign > 0) {
            out_coefficient = 0x7FFF;
            out_exponent = static_cast<int16_t>(1 - normalized_exponent);
        } else {
            out_coefficient = static_cast<int16_t>(-0x4000);
            out_exponent = static_cast<int16_t>(2 - normalized_exponent);
        }
        return;
    }

    const double normalized_value = static_cast<double>(magnitude) / kQ15Scale;
    const double inverse = 1.0 / normalized_value;
    const long long magnitude_coeff = std::clamp<long long>(
        std::llround((std::fabs(inverse) * kQ15Scale) * 0.5), 0LL, 32767LL);
    out_coefficient = static_cast<int16_t>(sign > 0 ? magnitude_coeff : -magnitude_coeff);
    out_exponent = static_cast<int16_t>(1 - normalized_exponent);
}

int16_t addS16(int16_t a, int16_t b) {
    return static_cast<int16_t>(static_cast<int32_t>(a) + static_cast<int32_t>(b));
}

int16_t subS16(int16_t a, int16_t b) {
    return static_cast<int16_t>(static_cast<int32_t>(a) - static_cast<int32_t>(b));
}

int32_t mulQ15Wide(int16_t a, int16_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * static_cast<int64_t>(b)) >> 15);
}

void normalizeSigned16(int16_t value, int16_t& coefficient, int16_t& exponent) {
    coefficient = value;
    if (value == 0) {
        exponent = static_cast<int16_t>(exponent - 15);
        return;
    }

    const int shift = normalizedShift16(value);
    if (shift > 0) {
        coefficient = static_cast<int16_t>(static_cast<int32_t>(value) << shift);
        exponent = static_cast<int16_t>(exponent - shift);
    }
}

int16_t truncateSigned16(int16_t coefficient, int16_t exponent) {
    if (exponent > 0) {
        if (coefficient > 0) return 32767;
        if (coefficient < 0) return -32767;
        return 0;
    }
    if (exponent < -15) {
        return 0;
    }
    if (exponent >= 0) {
        return coefficient;
    }

    return static_cast<int16_t>(static_cast<int32_t>(coefficient) >> (-exponent));
}
}

DSP1::DSP1(MapType map_type) : map_type_(map_type) {
    switch (map_type_) {
    case MapType::HiROM:
        boundary_ = 0x7000;
        break;
    case MapType::LoROMLarge:
        boundary_ = 0x4000;
        break;
    case MapType::LoROMSmall:
        boundary_ = 0xC000;
        break;
    default:
        boundary_ = 0xFFFF;
        break;
    }

    matrix_a_[0][0] = matrix_b_[0][0] = matrix_c_[0][0] = 0x4000;
    matrix_a_[1][1] = matrix_b_[1][1] = matrix_c_[1][1] = 0x4000;
    matrix_a_[2][2] = matrix_b_[2][2] = matrix_c_[2][2] = 0x4000;
}

bool DSP1::handlesAddress(uint8_t bank, uint16_t offset) const {
    switch (map_type_) {
    case MapType::HiROM:
        return (((bank <= 0x1F) || (bank >= 0x80 && bank <= 0x9F)) &&
            offset >= 0x6000 && offset <= 0x7FFF);
    case MapType::LoROMSmall:
        return (((bank >= 0x20 && bank <= 0x3F) || (bank >= 0xA0 && bank <= 0xBF)) &&
            offset >= 0x8000);
    case MapType::LoROMLarge:
        return (((bank >= 0x60 && bank <= 0x6F) || (bank >= 0xE0 && bank <= 0xEF)) &&
            offset < 0x8000);
    default:
        return false;
    }
}

const char* DSP1::getMapTypeName() const {
    switch (map_type_) {
    case MapType::HiROM: return "HiROM";
    case MapType::LoROMSmall: return "LoROM-S";
    case MapType::LoROMLarge: return "LoROM-L";
    default: return "None";
    }
}

void DSP1::dumpProjectionState(std::ostream& out) const {
    out << "[DSP1-PROJ]"
        << " focus=$" << std::hex
        << static_cast<int16_t>(projection_.focus_x) << ",$" << static_cast<int16_t>(projection_.focus_y)
        << ",$" << static_cast<int16_t>(projection_.focus_z)
        << " center=$" << projection_.center_x_i << ",$" << projection_.center_y_i
        << ",$" << projection_.center_z_i
        << " eye=$" << projection_.gx << ",$" << projection_.gy << ",$" << projection_.gz
        << " lfe=$" << static_cast<int16_t>(projection_.screen_distance)
        << " les=$" << static_cast<int16_t>(projection_.eye_distance)
        << " clipped_azs=$" << projection_.clipped_azimuth
        << " sin_aas=$" << projection_.sin_aas
        << " cos_aas=$" << projection_.cos_aas
        << " sin_azs=$" << projection_.sin_azs
        << " cos_azs=$" << projection_.cos_azs
        << " sin_clip=$" << projection_.sin_azs_clipped
        << " cos_clip=$" << projection_.cos_azs_clipped
        << " nx=$" << projection_.nx
        << " ny=$" << projection_.ny
        << " nz=$" << projection_.nz
        << " vplane=$" << projection_.vplane_c << "^" << projection_.vplane_e
        << " voffset=$" << projection_.voffset_i
        << " sec1=$" << projection_.sec_azs_c1 << "^" << projection_.sec_azs_e1
        << " sec2=$" << projection_.sec_azs_c2 << "^" << projection_.sec_azs_e2
        << std::dec << "\n";
}

void DSP1::resetInterface() {
    waiting_for_command_ = true;
    current_command_ = 0;
    input_expected_bytes_ = 0;
    input_index_ = 0;
    output_index_ = 0;
    output_count_ = 0;
    raster_stream_latched_ = false;
}

uint8_t DSP1::cpuRead(uint16_t offset) {
    if (offset >= boundary_) {
        status_read_count_++;
        return 0x80;
    }

    data_read_count_++;
    if (output_index_ < output_count_) {
        const uint8_t value = output_bytes_[output_index_++];
        if (output_index_ >= output_count_ && (current_command_ == 0x0A || current_command_ == 0x1A)) {
            refillRasterOutput();
        }
        return value;
    }
    return 0x80;
}

void DSP1::cpuWrite(uint16_t offset, uint8_t data) {
    if (offset >= boundary_) {
        return;
    }

    data_write_count_++;

    if ((current_command_ == 0x0A || current_command_ == 0x1A) && output_index_ < output_count_) {
        output_index_++;
        if (output_index_ >= output_count_) {
            refillRasterOutput();
        }
        return;
    }

    if (waiting_for_command_) {
        beginCommand(data);
        return;
    }

    if (input_index_ < input_bytes_.size()) {
        input_bytes_[input_index_++] = data;
    }

    if (input_index_ >= input_expected_bytes_) {
        executeCommand();
    }
}

void DSP1::beginCommand(uint8_t command) {
    if (command == 0x80) {
        resetInterface();
        return;
    }

    waiting_for_command_ = false;
    current_command_ = command;
    input_index_ = 0;
    output_index_ = 0;
    output_count_ = 0;
    raster_stream_latched_ = false;
    input_expected_bytes_ = expectedBytesForCommand(command);

    command_count_++;
    recent_commands_[recent_command_pos_] = current_command_;
    recent_command_pos_ = (recent_command_pos_ + 1) % recent_commands_.size();

    if (input_expected_bytes_ == 0) {
        unsupported_command_count_++;
        captureLastIO();
        waiting_for_command_ = true;
    }
}

std::size_t DSP1::expectedBytesForCommand(uint8_t command) {
    switch (command) {
    case 0x00: return 4;
    case 0x10:
    case 0x20:
    case 0x30: return 4;
    case 0x04:
    case 0x24: return 4;
    case 0x08: return 6;
    case 0x18:
    case 0x38: return 8;
    case 0x28: return 6;
    case 0x0C:
    case 0x2C: return 6;
    case 0x1C:
    case 0x3C: return 12;
    case 0x02:
    case 0x12:
    case 0x22:
    case 0x32: return 14;
    case 0x0A: return 2;
    case 0x1A:
    case 0x2A:
    case 0x3A:
        current_command_ = 0x1A;
        return 2;
    case 0x06:
    case 0x16:
    case 0x26:
    case 0x36: return 6;
    case 0x0E:
    case 0x1E:
    case 0x2E:
    case 0x3E: return 4;
    case 0x01:
    case 0x05:
    case 0x31:
    case 0x35: return 8;
    case 0x11:
    case 0x15: return 8;
    case 0x21:
    case 0x25: return 8;
    case 0x0D:
    case 0x09:
    case 0x39:
    case 0x3D: return 6;
    case 0x1D:
    case 0x19: return 6;
    case 0x2D:
    case 0x29: return 6;
    case 0x03:
    case 0x13:
    case 0x23: return 6;
    case 0x0B:
    case 0x1B:
    case 0x2B:
    case 0x3B: return 6;
    case 0x14:
    case 0x34: return 12;
    case 0x07:
    case 0x0F: return 2;
    case 0x27:
    case 0x2F: return 2;
    case 0x17:
    case 0x37:
    case 0x3F:
        current_command_ = 0x1F;
        return 2;
    case 0x1F: return 2;
    default: return 0;
    }
}

uint16_t DSP1::readInputWord(std::size_t byte_index) const {
    if (byte_index + 1 >= input_bytes_.size()) return 0;
    return static_cast<uint16_t>(input_bytes_[byte_index]) |
        (static_cast<uint16_t>(input_bytes_[byte_index + 1]) << 8);
}

void DSP1::pushOutputRaw(uint8_t value) {
    if (output_count_ < output_bytes_.size()) {
        output_bytes_[output_count_++] = value;
    }
}

void DSP1::pushOutputWord(int16_t value) {
    pushOutputRaw(static_cast<uint8_t>(value & 0xFF));
    pushOutputRaw(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void DSP1::captureLastIO() {
    last_input_words_.fill(0);
    last_output_words_.fill(0);
    last_input_word_count_ = std::min<std::size_t>(last_input_words_.size(), input_index_ / 2);
    last_output_word_count_ = std::min<std::size_t>(last_output_words_.size(), output_count_ / 2);
    for (std::size_t i = 0; i < last_input_word_count_; i++) {
        last_input_words_[i] = readInputWord(i * 2);
    }
    for (std::size_t i = 0; i < last_output_word_count_; i++) {
        const std::size_t byte_index = i * 2;
        last_output_words_[i] = static_cast<uint16_t>(output_bytes_[byte_index]) |
            (static_cast<uint16_t>(output_bytes_[byte_index + 1]) << 8);
    }

    CommandTrace& trace = command_trace_[command_trace_pos_];
    trace.command = last_command_;
    trace.input_words = last_input_words_;
    trace.output_words = last_output_words_;
    trace.input_word_count = last_input_word_count_;
    trace.output_word_count = last_output_word_count_;
    command_trace_pos_ = (command_trace_pos_ + 1) % command_trace_.size();
}

void DSP1::refillRasterOutput() {
    output_index_ = 0;
    output_count_ = 0;
    execRaster();
    last_command_ = current_command_;
    captureLastIO();
}

void DSP1::executeCommand() {
    waiting_for_command_ = true;
    output_index_ = 0;
    output_count_ = 0;
    last_command_ = current_command_;

    switch (current_command_) {
    case 0x00:
        pushOutputWord(execMultiply(static_cast<int16_t>(readInputWord(0)), static_cast<int16_t>(readInputWord(2)), false));
        break;
    case 0x20:
        pushOutputWord(execMultiply(static_cast<int16_t>(readInputWord(0)), static_cast<int16_t>(readInputWord(2)), true));
        break;
    case 0x10:
    case 0x30:
        execInverse();
        break;
    case 0x04:
    case 0x24:
        execTrig(current_command_ == 0x24);
        break;
    case 0x08:
        execRadiusSquared();
        break;
    case 0x18:
        execRange(false);
        break;
    case 0x38:
        execRange(true);
        break;
    case 0x28:
        execDistance();
        break;
    case 0x0C:
    case 0x2C:
        execRotate2D();
        break;
    case 0x02:
    case 0x12:
    case 0x22:
    case 0x32:
        execSetProjection();
        break;
    case 0x0A:
    case 0x1A:
        execRaster();
        break;
    case 0x06:
    case 0x16:
    case 0x26:
    case 0x36:
        execProject();
        break;
    case 0x0E:
    case 0x1E:
    case 0x2E:
    case 0x3E:
        execTarget();
        break;
    case 0x01:
    case 0x05:
    case 0x31:
    case 0x35:
        execMatrixA();
        break;
    case 0x11:
    case 0x15:
        execMatrixB();
        break;
    case 0x21:
    case 0x25:
        execMatrixC();
        break;
    case 0x0D:
    case 0x09:
    case 0x39:
    case 0x3D:
        execTransformA();
        break;
    case 0x1D:
    case 0x19:
        execTransformB();
        break;
    case 0x2D:
    case 0x29:
        execTransformC();
        break;
    case 0x03:
        execReverseTransformA();
        break;
    case 0x13:
        execReverseTransformB();
        break;
    case 0x23:
        execReverseTransformC();
        break;
    case 0x0B:
    case 0x3B:
        execScalarA();
        break;
    case 0x1B:
        execScalarB();
        break;
    case 0x2B:
        execScalarC();
        break;
    case 0x14:
    case 0x34:
        execAttitude();
        break;
    case 0x07:
    case 0x0F:
        execMemoryTest();
        break;
    case 0x1F:
        execRomTable();
        break;
    case 0x27:
    case 0x2F:
        pushOutputWord(0);
        break;
    default:
        unsupported_command_count_++;
        break;
    }

    captureLastIO();
}

int16_t DSP1::execMultiply(int16_t a, int16_t b, bool bias_one) const {
    long long result = (static_cast<long long>(a) * static_cast<long long>(b)) >> 15;
    if (bias_one) result += 1;
    return clampS16(result);
}

void DSP1::execInverse() {
    const int16_t coefficient = static_cast<int16_t>(readInputWord(0));
    const int16_t exponent = static_cast<int16_t>(readInputWord(2));
    int16_t out_coefficient = 0;
    int16_t out_exponent = 0;
    inverseCoefficient(coefficient, exponent, out_coefficient, out_exponent);
    pushOutputWord(out_coefficient);
    pushOutputWord(out_exponent);
}

void DSP1::execTrig(bool alt_variant) {
    const int16_t angle = static_cast<int16_t>(readInputWord(0));
    const int16_t radius = static_cast<int16_t>(readInputWord(2));
    const int16_t sine = execMultiply(q15Sin(angle), radius, false);
    int16_t cosine = execMultiply(q15Cos(angle), radius, false);
    if (alt_variant) cosine = clampS16(static_cast<long long>(cosine) + 1LL);
    pushOutputWord(sine);
    pushOutputWord(cosine);
}

void DSP1::execRadiusSquared() {
    const long long x = static_cast<int16_t>(readInputWord(0));
    const long long y = static_cast<int16_t>(readInputWord(2));
    const long long z = static_cast<int16_t>(readInputWord(4));
    const uint32_t size = static_cast<uint32_t>(((x * x) + (y * y) + (z * z)) << 1);
    pushOutputWord(static_cast<int16_t>(size & 0xFFFFu));
    pushOutputWord(static_cast<int16_t>((size >> 16) & 0xFFFFu));
}

void DSP1::execRange(bool bias_one) {
    const long long x = static_cast<int16_t>(readInputWord(0));
    const long long y = static_cast<int16_t>(readInputWord(2));
    const long long z = static_cast<int16_t>(readInputWord(4));
    const long long r = static_cast<int16_t>(readInputWord(6));
    long long value = ((x * x) + (y * y) + (z * z) - (r * r)) >> 15;
    if (bias_one) value += 1;
    pushOutputWord(clampS16(value));
}

void DSP1::execDistance() {
    const double x = static_cast<double>(static_cast<int16_t>(readInputWord(0)));
    const double y = static_cast<double>(static_cast<int16_t>(readInputWord(2)));
    const double z = static_cast<double>(static_cast<int16_t>(readInputWord(4)));
    pushOutputWord(clampFloatToS16(std::sqrt((x * x) + (y * y) + (z * z))));
}

void DSP1::execRotate2D() {
    const int16_t angle = static_cast<int16_t>(readInputWord(0));
    const int16_t x1 = static_cast<int16_t>(readInputWord(2));
    const int16_t y1 = static_cast<int16_t>(readInputWord(4));
    const int16_t sine = q15Sin(angle);
    const int16_t cosine = q15Cos(angle);
    const int16_t x2 = static_cast<int16_t>(
        static_cast<int32_t>(mulQ15(y1, sine)) + static_cast<int32_t>(mulQ15(x1, cosine)));
    const int16_t y2 = static_cast<int16_t>(
        static_cast<int32_t>(mulQ15(y1, cosine)) - static_cast<int32_t>(mulQ15(x1, sine)));
    pushOutputWord(x2);
    pushOutputWord(y2);
}

void DSP1::execSetProjection() {
    const int16_t fx = static_cast<int16_t>(readInputWord(0));
    const int16_t fy = static_cast<int16_t>(readInputWord(2));
    const int16_t fz = static_cast<int16_t>(readInputWord(4));
    const int16_t lfe = static_cast<int16_t>(readInputWord(6));
    const int16_t les = static_cast<int16_t>(readInputWord(8));
    const int16_t aas = static_cast<int16_t>(readInputWord(10));
    const int16_t azs = static_cast<int16_t>(readInputWord(12));
    int16_t clipped_azs = azs;

    projection_.focus_x = fx;
    projection_.focus_y = fy;
    projection_.focus_z = fz;
    projection_.screen_distance = lfe;
    projection_.eye_distance = les;
    projection_.azimuth = angleToRadians(aas);
    projection_.zenith = angleToRadians(azs);

    projection_.sin_aas = q15Sin(aas);
    projection_.cos_aas = q15Cos(aas);
    projection_.sin_azs = q15Sin(azs);
    projection_.cos_azs = q15Cos(azs);

    projection_.nx = mulQ15(projection_.sin_azs, static_cast<int16_t>(-projection_.sin_aas));
    projection_.ny = mulQ15(projection_.sin_azs, projection_.cos_aas);
    projection_.nz = projection_.cos_azs;

    projection_.center_x_i = addS16(fx, mulQ15(lfe, projection_.nx));
    projection_.center_y_i = addS16(fy, mulQ15(lfe, projection_.ny));
    projection_.center_z_i = addS16(fz, mulQ15(lfe, projection_.nz));

    projection_.gx = subS16(projection_.center_x_i, mulQ15(les, projection_.nx));
    projection_.gy = subS16(projection_.center_y_i, mulQ15(les, projection_.ny));
    projection_.gz = subS16(projection_.center_z_i, mulQ15(les, projection_.nz));

    normalizeFixed(les, projection_.c_les, projection_.e_les);
    projection_.g_les = les;
    normalizeFixed(projection_.center_z_i, projection_.vplane_c, projection_.vplane_e);

    const int depth_shift = std::clamp(-projection_.vplane_e, 0, 15);
    int16_t max_zenith = kMaxZenithByExponent[depth_shift];
    if (clipped_azs < 0) {
        max_zenith = -max_zenith;
        if (clipped_azs < max_zenith + 1) {
            clipped_azs = static_cast<int16_t>(max_zenith + 1);
        }
    } else if (clipped_azs > max_zenith) {
        clipped_azs = max_zenith;
    }

    projection_.clipped_zenith = angleToRadians(clipped_azs);
    projection_.clipped_azimuth = clipped_azs;
    projection_.sin_azs_clipped = q15Sin(clipped_azs);
    projection_.cos_azs_clipped = q15Cos(clipped_azs);

    inverseCoefficient(projection_.cos_azs_clipped, 0, projection_.sec_azs_c1, projection_.sec_azs_e1);
    int16_t coeff = static_cast<int16_t>(mulQ15Wide(projection_.vplane_c, projection_.sec_azs_c1));
    int16_t exp = projection_.vplane_e;
    normalizeSigned16(coeff, coeff, exp);
    exp = static_cast<int16_t>(exp + projection_.sec_azs_e1);
    const int16_t center_adjust = mulQ15(truncateSigned16(coeff, exp), projection_.sin_azs_clipped);

    projection_.center_x_i = addS16(projection_.center_x_i, mulQ15(center_adjust, projection_.sin_aas));
    projection_.center_y_i = subS16(projection_.center_y_i, mulQ15(center_adjust, projection_.cos_aas));

    projection_.center_x = projection_.center_x_i;
    projection_.center_y = projection_.center_y_i;
    projection_.center_z = projection_.center_z_i;
    projection_.eye_x = projection_.gx;
    projection_.eye_y = projection_.gy;
    projection_.eye_z = projection_.gz;
    projection_.horizontal_x = q15ToUnit(projection_.cos_aas);
    projection_.horizontal_y = q15ToUnit(projection_.sin_aas);
    projection_.horizontal_z = 0.0;
    projection_.vertical_x = q15ToUnit(mulQ15(projection_.cos_azs, static_cast<int16_t>(-projection_.sin_aas)));
    projection_.vertical_y = q15ToUnit(mulQ15(projection_.cos_azs, projection_.cos_aas));
    projection_.vertical_z = q15ToUnit(static_cast<int16_t>(-projection_.sin_azs));
    projection_.normal_x = q15ToUnit(projection_.nx);
    projection_.normal_y = q15ToUnit(projection_.ny);
    projection_.normal_z = q15ToUnit(projection_.nz);

    int16_t vof = 0;
    if (azs != clipped_azs || azs == max_zenith) {
        int16_t unclipped = azs;
        if (unclipped == std::numeric_limits<int16_t>::min()) {
            unclipped = -32767;
        }

        int16_t aux = static_cast<int16_t>(unclipped - max_zenith);
        if (aux >= 0) {
            aux = static_cast<int16_t>(aux - 1);
        }
        aux = static_cast<int16_t>(~(aux << 2));

        int16_t curve = mulQ15(aux, kClipVOffsetCurveB);
        curve = addS16(mulQ15(curve, aux), kClipVOffsetCurveA);
        vof = subS16(vof, mulQ15(mulQ15(curve, aux), les));

        const int16_t aux_sq = mulQ15(aux, aux);
        const int16_t cos_curve = addS16(mulQ15(aux_sq, kClipCosCurveB), kClipCosCurveA);
        projection_.cos_azs_clipped = addS16(
            projection_.cos_azs_clipped,
            mulQ15(mulQ15(aux_sq, cos_curve), projection_.cos_azs_clipped));
    }

    projection_.voffset_i = mulQ15(les, projection_.cos_azs_clipped);
    projection_.vertical_offset = projection_.voffset_i;
    projection_.plane_depth = projection_.center_z_i;

    int16_t csec_coeff = 0;
    int16_t csec_exp = 0;
    inverseCoefficient(projection_.sin_azs_clipped, 0, csec_coeff, csec_exp);
    coeff = projection_.voffset_i;
    exp = 0;
    normalizeSigned16(coeff, coeff, exp);
    coeff = static_cast<int16_t>(mulQ15Wide(coeff, csec_coeff));
    normalizeSigned16(coeff, coeff, exp);
    exp = static_cast<int16_t>(exp + csec_exp);
    if (coeff == -32768) {
        coeff >>= 1;
        exp = static_cast<int16_t>(exp + 1);
    }

    inverseCoefficient(projection_.cos_azs_clipped, 0, projection_.sec_azs_c2, projection_.sec_azs_e2);

    const int16_t vva = truncateSigned16(static_cast<int16_t>(-coeff), exp);
    const int16_t cx = projection_.center_x_i;
    const int16_t cy = projection_.center_y_i;

    pushOutputWord(vof);
    pushOutputWord(vva);
    pushOutputWord(cx);
    pushOutputWord(cy);
}

void DSP1::execRaster() {
    if (!raster_stream_latched_ && input_index_ >= 2) {
        projection_.raster_scanline = static_cast<int16_t>(readInputWord(0));
        raster_stream_latched_ = true;
    }

    const int16_t scanline = projection_.raster_scanline;
    const int16_t denom = addS16(mulQ15(scanline, projection_.sin_azs), projection_.voffset_i);

    int16_t inv_coeff = 0;
    int16_t inv_exp = 0;
    inverseCoefficient(denom, 7, inv_coeff, inv_exp);

    const int16_t base = static_cast<int16_t>(mulQ15Wide(inv_coeff, projection_.vplane_c));

    int16_t coeff = base;
    int16_t exp = static_cast<int16_t>(inv_exp + projection_.vplane_e);
    normalizeSigned16(coeff, coeff, exp);
    const int16_t plane = truncateSigned16(coeff, exp);

    coeff = static_cast<int16_t>(mulQ15Wide(base, projection_.sec_azs_c2));
    exp = static_cast<int16_t>(inv_exp + projection_.vplane_e + projection_.sec_azs_e2);
    normalizeSigned16(coeff, coeff, exp);
    const int16_t skew = truncateSigned16(coeff, exp);

    pushOutputWord(mulQ15(plane, projection_.cos_aas));
    pushOutputWord(mulQ15(skew, static_cast<int16_t>(-projection_.sin_aas)));
    pushOutputWord(mulQ15(plane, projection_.sin_aas));
    pushOutputWord(mulQ15(skew, projection_.cos_aas));

    projection_.raster_scanline = static_cast<int16_t>(projection_.raster_scanline + 1);
}

void DSP1::execProject() {
    const double x = static_cast<int16_t>(readInputWord(0));
    const double y = static_cast<int16_t>(readInputWord(2));
    const double z = static_cast<int16_t>(readInputWord(4));
    const double dx = x - projection_.eye_x;
    const double dy = y - projection_.eye_y;
    const double dz = z - projection_.eye_z;

    double depth = projection_.eye_distance +
        ((dx * projection_.normal_x) + (dy * projection_.normal_y) + (dz * projection_.normal_z));
    if (std::fabs(depth) < kMinDepth) depth = depth < 0.0 ? -kMinDepth : kMinDepth;

    const double scale = projection_.eye_distance / depth;
    const double h = (((dx * projection_.horizontal_x) + (dy * projection_.horizontal_y) + (dz * projection_.horizontal_z)) * scale) * 128.0;
    const double v = (((dx * projection_.vertical_x) + (dy * projection_.vertical_y) + (dz * projection_.vertical_z)) * scale) * 128.0;
    const double m = scale * 128.0;

    pushOutputWord(clampFloatToS16(h));
    pushOutputWord(clampFloatToS16(v));
    pushOutputWord(clampFloatToS16(m));
}

void DSP1::execTarget() {
    const int16_t h = static_cast<int16_t>(readInputWord(0));
    const int16_t v = static_cast<int16_t>(readInputWord(2));
    const int16_t denom = addS16(mulQ15(v, projection_.sin_azs), projection_.voffset_i);

    int16_t inv_coeff = 0;
    int16_t inv_exp = 0;
    inverseCoefficient(denom, 8, inv_coeff, inv_exp);

    const int16_t base = static_cast<int16_t>(mulQ15Wide(inv_coeff, projection_.vplane_c));

    int16_t coeff = base;
    int16_t exp = static_cast<int16_t>(inv_exp + projection_.vplane_e);
    normalizeSigned16(coeff, coeff, exp);
    int16_t plane = truncateSigned16(coeff, exp);
    plane = mulQ15(plane, static_cast<int16_t>(static_cast<int32_t>(h) << 8));

    int16_t x = addS16(projection_.center_x_i, mulQ15(plane, projection_.cos_aas));
    int16_t y = subS16(projection_.center_y_i, mulQ15(plane, projection_.sin_aas));

    coeff = static_cast<int16_t>(mulQ15Wide(base, projection_.sec_azs_c1));
    exp = static_cast<int16_t>(inv_exp + projection_.vplane_e + projection_.sec_azs_e1);
    normalizeSigned16(coeff, coeff, exp);
    int16_t vertical = truncateSigned16(coeff, exp);
    vertical = mulQ15(vertical, static_cast<int16_t>(static_cast<int32_t>(v) << 8));

    x = addS16(x, mulQ15(vertical, static_cast<int16_t>(-projection_.sin_aas)));
    y = addS16(y, mulQ15(vertical, projection_.cos_aas));

    pushOutputWord(x);
    pushOutputWord(y);
}

void DSP1::buildMatrix(std::array<std::array<int16_t, 3>, 3>& matrix, int16_t scale, int16_t zr, int16_t yr, int16_t xr) {
    const double m = static_cast<double>(scale) * 0.5;
    const double sin_z = std::sin(angleToRadians(zr));
    const double cos_z = std::cos(angleToRadians(zr));
    const double sin_y = std::sin(angleToRadians(yr));
    const double cos_y = std::cos(angleToRadians(yr));
    const double sin_x = std::sin(angleToRadians(xr));
    const double cos_x = std::cos(angleToRadians(xr));

    matrix[0][0] = clampFloatToS16(m * cos_z * cos_y);
    matrix[0][1] = clampFloatToS16(-m * sin_z * cos_y);
    matrix[0][2] = clampFloatToS16(m * sin_y);

    matrix[1][0] = clampFloatToS16((m * sin_z * cos_x) + (m * cos_z * sin_x * sin_y));
    matrix[1][1] = clampFloatToS16((m * cos_z * cos_x) - (m * sin_z * sin_x * sin_y));
    matrix[1][2] = clampFloatToS16(-m * sin_x * cos_y);

    matrix[2][0] = clampFloatToS16((m * sin_z * sin_x) - (m * cos_z * cos_x * sin_y));
    matrix[2][1] = clampFloatToS16((m * cos_z * sin_x) + (m * sin_z * cos_x * sin_y));
    matrix[2][2] = clampFloatToS16(m * cos_x * cos_y);
}

void DSP1::execMatrixA() {
    buildMatrix(matrix_a_,
        static_cast<int16_t>(readInputWord(0)),
        static_cast<int16_t>(readInputWord(2)),
        static_cast<int16_t>(readInputWord(4)),
        static_cast<int16_t>(readInputWord(6)));
}

void DSP1::execMatrixB() {
    buildMatrix(matrix_b_,
        static_cast<int16_t>(readInputWord(0)),
        static_cast<int16_t>(readInputWord(2)),
        static_cast<int16_t>(readInputWord(4)),
        static_cast<int16_t>(readInputWord(6)));
}

void DSP1::execMatrixC() {
    buildMatrix(matrix_c_,
        static_cast<int16_t>(readInputWord(0)),
        static_cast<int16_t>(readInputWord(2)),
        static_cast<int16_t>(readInputWord(4)),
        static_cast<int16_t>(readInputWord(6)));
}

void DSP1::forwardTransform(const std::array<std::array<int16_t, 3>, 3>& matrix) {
    const int16_t x = static_cast<int16_t>(readInputWord(0));
    const int16_t y = static_cast<int16_t>(readInputWord(2));
    const int16_t z = static_cast<int16_t>(readInputWord(4));
    for (int row = 0; row < 3; row++) {
        const long long value =
            static_cast<long long>(mulQ15(x, matrix[row][0])) +
            static_cast<long long>(mulQ15(y, matrix[row][1])) +
            static_cast<long long>(mulQ15(z, matrix[row][2]));
        pushOutputWord(clampS16(value));
    }
}

void DSP1::reverseTransform(const std::array<std::array<int16_t, 3>, 3>& matrix) {
    const int16_t f = static_cast<int16_t>(readInputWord(0));
    const int16_t l = static_cast<int16_t>(readInputWord(2));
    const int16_t u = static_cast<int16_t>(readInputWord(4));
    for (int col = 0; col < 3; col++) {
        const long long value =
            static_cast<long long>(mulQ15(f, matrix[0][col])) +
            static_cast<long long>(mulQ15(l, matrix[1][col])) +
            static_cast<long long>(mulQ15(u, matrix[2][col]));
        pushOutputWord(clampS16(value));
    }
}

void DSP1::scalarTransform(const std::array<std::array<int16_t, 3>, 3>& matrix) {
    const int16_t x = static_cast<int16_t>(readInputWord(0));
    const int16_t y = static_cast<int16_t>(readInputWord(2));
    const int16_t z = static_cast<int16_t>(readInputWord(4));
    const long long value =
        static_cast<long long>(x) * matrix[0][0] +
        static_cast<long long>(y) * matrix[0][1] +
        static_cast<long long>(z) * matrix[0][2];
    pushOutputWord(clampS16(value >> 15));
}

void DSP1::execTransformA() { forwardTransform(matrix_a_); }
void DSP1::execTransformB() { forwardTransform(matrix_b_); }
void DSP1::execTransformC() { forwardTransform(matrix_c_); }
void DSP1::execReverseTransformA() { reverseTransform(matrix_a_); }
void DSP1::execReverseTransformB() { reverseTransform(matrix_b_); }
void DSP1::execReverseTransformC() { reverseTransform(matrix_c_); }
void DSP1::execScalarA() { scalarTransform(matrix_a_); }
void DSP1::execScalarB() { scalarTransform(matrix_b_); }
void DSP1::execScalarC() { scalarTransform(matrix_c_); }

void DSP1::execAttitude() {
    const int16_t zr = static_cast<int16_t>(readInputWord(0));
    const int16_t xr = static_cast<int16_t>(readInputWord(2));
    const int16_t yr = static_cast<int16_t>(readInputWord(4));
    const int16_t u = static_cast<int16_t>(readInputWord(6));
    const int16_t f = static_cast<int16_t>(readInputWord(8));
    const int16_t l = static_cast<int16_t>(readInputWord(10));

    const int16_t sin_x = q15Sin(xr);
    const int16_t cos_x = q15Cos(xr);
    const int16_t sin_y = q15Sin(yr);
    const int16_t cos_y = q15Cos(yr);

    const double sin_x_unit = q15ToUnit(sin_x);
    const double cos_x_unit = q15ToUnit(cos_x);
    const double sin_y_unit = q15ToUnit(sin_y);
    const double cos_y_unit = q15ToUnit(cos_y);

    const double z_term = (static_cast<double>(u) * cos_y_unit) -
        (static_cast<double>(f) * sin_y_unit);
    const double x_term = (static_cast<double>(u) * sin_y_unit) +
        (static_cast<double>(f) * cos_y_unit);
    const double y_term = -safeDiv(
        ((static_cast<double>(u) * cos_y_unit) + (static_cast<double>(f) * sin_y_unit)) * sin_x_unit,
        cos_x_unit);

    pushOutputWord(clampFloatToS16(static_cast<double>(zr) + safeDiv(z_term, cos_x_unit)));
    pushOutputWord(clampFloatToS16(static_cast<double>(xr) + x_term));
    pushOutputWord(clampFloatToS16(static_cast<double>(yr) + y_term + static_cast<double>(l)));
}

void DSP1::execMemoryTest() {
    pushOutputWord(0);
}

void DSP1::execRomTable() {
    const uint16_t start = readInputWord(0);
    output_index_ = 0;
    output_count_ = 0;
    for (int i = 0; i < 1024; i++) {
        const uint16_t value = static_cast<uint16_t>((start + i) ^ 0xFFFFu);
        pushOutputRaw(static_cast<uint8_t>(value & 0xFFu));
        pushOutputRaw(static_cast<uint8_t>((value >> 8) & 0xFFu));
    }
}
