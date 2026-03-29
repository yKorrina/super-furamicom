#ifndef DSP1_HPP
#define DSP1_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>

class DSP1 {
public:
    struct CommandTrace {
        uint8_t command = 0;
        std::array<uint16_t, 8> input_words{};
        std::array<uint16_t, 8> output_words{};
        std::size_t input_word_count = 0;
        std::size_t output_word_count = 0;
    };

    enum class MapType {
        None,
        LoROMSmall,
        LoROMLarge,
        HiROM,
    };

    explicit DSP1(MapType map_type);

    bool handlesAddress(uint8_t bank, uint16_t offset) const;
    uint8_t cpuRead(uint16_t offset);
    void cpuWrite(uint16_t offset, uint8_t data);

    const char* getMapTypeName() const;
    MapType getMapType() const { return map_type_; }
    uint16_t getBoundary() const { return boundary_; }

    bool isWaitingForCommand() const { return waiting_for_command_; }
    uint8_t getCurrentCommand() const { return current_command_; }
    uint8_t getLastCommand() const { return last_command_; }
    uint32_t getCommandCount() const { return command_count_; }
    uint32_t getUnsupportedCommandCount() const { return unsupported_command_count_; }
    uint32_t getDataReadCount() const { return data_read_count_; }
    uint32_t getStatusReadCount() const { return status_read_count_; }
    uint32_t getDataWriteCount() const { return data_write_count_; }
    std::size_t getPendingInputBytes() const { return input_expected_bytes_ > input_index_ ? (input_expected_bytes_ - input_index_) : 0; }
    std::size_t getPendingOutputBytes() const { return output_count_ > output_index_ ? (output_count_ - output_index_) : 0; }
    const std::array<uint8_t, 8>& getRecentCommands() const { return recent_commands_; }
    const std::array<uint16_t, 8>& getLastInputWords() const { return last_input_words_; }
    const std::array<uint16_t, 8>& getLastOutputWords() const { return last_output_words_; }
    std::size_t getLastInputWordCount() const { return last_input_word_count_; }
    std::size_t getLastOutputWordCount() const { return last_output_word_count_; }
    static constexpr std::size_t kCommandTraceCount = 16;
    const std::array<CommandTrace, kCommandTraceCount>& getCommandTrace() const { return command_trace_; }
    std::size_t getCommandTracePos() const { return command_trace_pos_; }
    void dumpProjectionState(std::ostream& out) const;

private:
    struct ProjectionState {
        double focus_x = 0.0;
        double focus_y = 0.0;
        double focus_z = 0.0;
        double screen_distance = 0.0;
        double eye_distance = 0.0;
        double azimuth = 0.0;
        double zenith = 0.0;
        double clipped_zenith = 0.0;
        double normal_x = 0.0;
        double normal_y = 0.0;
        double normal_z = 1.0;
        double center_x = 0.0;
        double center_y = 0.0;
        double center_z = 1.0;
        double eye_x = 0.0;
        double eye_y = 0.0;
        double eye_z = 0.0;
        double horizontal_x = 1.0;
        double horizontal_y = 0.0;
        double horizontal_z = 0.0;
        double vertical_x = 0.0;
        double vertical_y = 1.0;
        double vertical_z = 0.0;
        double vertical_offset = 1.0;
        double plane_depth = 1.0;
        int16_t raster_scanline = 0;
        int16_t sin_aas = 0;
        int16_t cos_aas = 0;
        int16_t sin_azs = 0;
        int16_t cos_azs = 0;
        int16_t sin_azs_clipped = 0;
        int16_t cos_azs_clipped = 0;
        int16_t nx = 0;
        int16_t ny = 0;
        int16_t nz = 0;
        int16_t center_x_i = 0;
        int16_t center_y_i = 0;
        int16_t center_z_i = 0;
        int16_t gx = 0;
        int16_t gy = 0;
        int16_t gz = 0;
        int16_t c_les = 0;
        int16_t e_les = 0;
        int16_t g_les = 0;
        int16_t vplane_c = 0;
        int16_t vplane_e = 0;
        int16_t voffset_i = 0;
        int16_t sec_azs_c1 = 0;
        int16_t sec_azs_e1 = 0;
        int16_t sec_azs_c2 = 0;
        int16_t sec_azs_e2 = 0;
        int16_t clipped_azimuth = 0;
    };

    MapType map_type_ = MapType::None;
    uint16_t boundary_ = 0xFFFF;

    bool waiting_for_command_ = true;
    uint8_t current_command_ = 0;
    uint8_t last_command_ = 0;
    std::size_t input_expected_bytes_ = 0;
    std::size_t input_index_ = 0;
    std::size_t output_index_ = 0;
    std::size_t output_count_ = 0;
    bool raster_stream_latched_ = false;

    uint32_t command_count_ = 0;
    uint32_t unsupported_command_count_ = 0;
    uint32_t data_read_count_ = 0;
    uint32_t status_read_count_ = 0;
    uint32_t data_write_count_ = 0;

    std::array<uint8_t, 512> input_bytes_{};
    std::array<uint8_t, 512> output_bytes_{};
    std::array<uint8_t, 8> recent_commands_{};
    std::size_t recent_command_pos_ = 0;

    std::array<uint16_t, 8> last_input_words_{};
    std::array<uint16_t, 8> last_output_words_{};
    std::size_t last_input_word_count_ = 0;
    std::size_t last_output_word_count_ = 0;
    std::array<CommandTrace, kCommandTraceCount> command_trace_{};
    std::size_t command_trace_pos_ = 0;

    ProjectionState projection_{};
    std::array<std::array<int16_t, 3>, 3> matrix_a_{};
    std::array<std::array<int16_t, 3>, 3> matrix_b_{};
    std::array<std::array<int16_t, 3>, 3> matrix_c_{};

    void resetInterface();
    void beginCommand(uint8_t command);
    void executeCommand();
    void refillRasterOutput();
    void pushOutputWord(int16_t value);
    void pushOutputRaw(uint8_t value);
    uint16_t readInputWord(std::size_t byte_index) const;
    void captureLastIO();
    std::size_t expectedBytesForCommand(uint8_t command);

    int16_t execMultiply(int16_t a, int16_t b, bool bias_one) const;
    void execInverse();
    void execTrig(bool alt_variant);
    void execRadiusSquared();
    void execRange(bool bias_one);
    void execDistance();
    void execRotate2D();
    void execSetProjection();
    void execRaster();
    void execProject();
    void execTarget();
    void execMatrixA();
    void execMatrixB();
    void execMatrixC();
    void execTransformA();
    void execTransformB();
    void execTransformC();
    void execReverseTransformA();
    void execReverseTransformB();
    void execReverseTransformC();
    void execScalarA();
    void execScalarB();
    void execScalarC();
    void execAttitude();
    void execMemoryTest();
    void execRomTable();

    void buildMatrix(std::array<std::array<int16_t, 3>, 3>& matrix, int16_t scale, int16_t zr, int16_t yr, int16_t xr);
    void forwardTransform(const std::array<std::array<int16_t, 3>, 3>& matrix);
    void reverseTransform(const std::array<std::array<int16_t, 3>, 3>& matrix);
    void scalarTransform(const std::array<std::array<int16_t, 3>, 3>& matrix);
};

#endif
