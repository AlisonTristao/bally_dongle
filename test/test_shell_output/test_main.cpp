// Regression coverage for lib/ShellOutput's String->std::string port
// (ESP-IDF migration phase 2, PLANO_ESPIDF_DONGLE.md). Not built before this
// port: ShellOutput had zero native test coverage (see the phase-2 survey),
// so these pin the trickiest logic -- ESP-NOW structured-line passthrough,
// leading-bracket-tag stripping, and the "! "-prefix framing -- against the
// exact byte sequences the pre-port Arduino::String implementation produced.

#include <ShellOutput.h>
#include <compat.h>
#include <unity.h>

#include <string>

namespace {

class RecordingIo : public ByteIO {
public:
    size_t write(const uint8_t* data, size_t len) override {
        out.append(reinterpret_cast<const char*>(data), len);
        return len;
    }
    int read() override { return -1; }
    int available() override { return 0; }
    explicit operator bool() const override { return true; }

    std::string out;
};

}  // namespace

void setUp() {}
void tearDown() {}

void test_write_line_wraps_in_cr_output_prefix_and_crlf() {
    RecordingIo io;
    ShellOutput::writeLine(io, "hello");
    TEST_ASSERT_EQUAL_STRING("\r! hello\r\n", io.out.c_str());
}

void test_print_tagged_applies_same_framing_as_write_line() {
    // printTagged's `tag` argument only steers classification (the ESP-NOW
    // passthrough check) -- it is never written to the wire itself, matching
    // the pre-port behaviour exactly.
    RecordingIo io;
    ShellOutput::printTagged(io, "mytag", "hi");
    TEST_ASSERT_EQUAL_STRING("\r! hi\r\n", io.out.c_str());
}

void test_print_tagged_strips_leading_bracket_tag_from_message() {
    RecordingIo io;
    ShellOutput::printTagged(io, "shell", "[help] usage: foo <bar>");
    TEST_ASSERT_EQUAL_STRING("\r! usage: foo <bar>\r\n", io.out.c_str());
}

void test_render_response_splits_lines_with_output_prefix() {
    const std::string rendered = ShellOutput::renderResponse("hello\nworld");
    TEST_ASSERT_EQUAL_STRING("\r! hello\r\n\r! world\r\n", rendered.c_str());
}

void test_render_response_passes_espnow_structured_line_through_raw() {
    const std::string rendered = ShellOutput::renderResponse("[info][some info]");
    TEST_ASSERT_EQUAL_STRING("\r[info][some info]\r\n", rendered.c_str());
}

void test_render_response_of_empty_text_is_empty() {
    const std::string rendered = ShellOutput::renderResponse("   \r\n  ");
    TEST_ASSERT_TRUE(rendered.empty());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_write_line_wraps_in_cr_output_prefix_and_crlf);
    RUN_TEST(test_print_tagged_applies_same_framing_as_write_line);
    RUN_TEST(test_print_tagged_strips_leading_bracket_tag_from_message);
    RUN_TEST(test_render_response_splits_lines_with_output_prefix);
    RUN_TEST(test_render_response_passes_espnow_structured_line_through_raw);
    RUN_TEST(test_render_response_of_empty_text_is_empty);
    return UNITY_END();
}
