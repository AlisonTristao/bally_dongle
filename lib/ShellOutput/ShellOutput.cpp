#include "ShellOutput.h"

#include <ShellStyle.h>

#include "string_compat.h"

#include <cstring>
#include <string_view>

namespace {

constexpr const char* kCommandPrefix = "$ ";
constexpr const char* kOutputPrefix = "! ";

bool isEspNowTypeToken(const std::string& token) {
    return strcompat::equalsIgnoreCase(token, "info") ||
           strcompat::equalsIgnoreCase(token, "cmd") ||
           strcompat::equalsIgnoreCase(token, "cmdo") ||
           strcompat::equalsIgnoreCase(token, "telemetry") ||
           strcompat::equalsIgnoreCase(token, "tele") ||
           strcompat::equalsIgnoreCase(token, "error") ||
           strcompat::equalsIgnoreCase(token, "erro") ||
           strcompat::equalsIgnoreCase(token, "debug") ||
           strcompat::equalsIgnoreCase(token, "debg") ||
           strcompat::equalsIgnoreCase(token, "pakg") ||
           strcompat::equalsIgnoreCase(token, "none");
}

std::string normalizeTag(const char* tag) {
    std::string out = (tag != nullptr) ? std::string(tag) : std::string("shell");
    strcompat::trim(out);
    if (out.empty()) {
        out = "shell";
    }

    if (strcompat::startsWith(out, "[")) {
        out.erase(0, 1);
    }
    if (strcompat::endsWith(out, "]")) {
        out.erase(out.size() - 1, 1);
    }

    strcompat::trim(out);
    if (out.empty()) {
        out = "shell";
    }

    return out;
}

bool isEspNowStructuredLine(const std::string& line) {
    std::string lower = line;
    strcompat::trim(lower);

    if (!strcompat::startsWith(lower, "[")) {
        return false;
    }

    const int firstClose = strcompat::indexOf(lower, ']');
    if (firstClose <= 1) {
        return false;
    }

    const std::string firstToken = lower.substr(1, static_cast<size_t>(firstClose) - 1);
    if (!isEspNowTypeToken(firstToken)) {
        return false;
    }

    return (firstClose + 1 < static_cast<int>(lower.size())) && lower[static_cast<size_t>(firstClose) + 1] == '[';
}

std::string stripLeadingBracketTags(const std::string& input) {
    std::string out = input;
    strcompat::trim(out);

    if (isEspNowStructuredLine(out)) {
        return out;
    }

    while (strcompat::startsWith(out, "[")) {
        const int close = strcompat::indexOf(out, ']');
        if (close <= 0) {
            break;
        }

        out = out.substr(static_cast<size_t>(close) + 1);
        strcompat::trim(out);
    }

    return out;
}

bool hasVisualPrefix(const char* line) {
    if (line == nullptr) {
        return false;
    }

    if (std::strncmp(line, kOutputPrefix, std::strlen(kOutputPrefix)) == 0 ||
        std::strncmp(line, kCommandPrefix, std::strlen(kCommandPrefix)) == 0 ||
        isEspNowStructuredLine(std::string(line))) {
        return true;
    }

    return false;
}

std::string normalizeNewlines(const char* text) {
    if (text == nullptr) {
        return std::string("");
    }

    return std::string(text);
}

// String counterpart of the per-line half of writeLine()/writeRawLine():
// appends `line` to `out` with the given prefix and a CR+LF terminator, the
// whole thing wrapped in the SGR sequence for `cls` (a no-op when the class is
// Output or the firmware was built without TINYSHELL_COLOR). Only the
// single-line, non-empty case renderResponse() needs.
void appendLineWithPrefix(std::string& out, const char* prefix, const std::string& line,
                          ShellMsgClass cls = ShellMsgClass::Output) {
    const char* sgr = shell_sgr(cls);
    out += '\r';
    if (sgr[0] != '\0') {
        out += sgr;
    }
    if (prefix != nullptr) {
        out += prefix;
    }
    out += line;
    if (sgr[0] != '\0') {
        out += shell_sgr_reset();
    }
    out += "\r\n";
}

void writeTextWithPrefix(ByteIO& io, const char* prefix, const char* text, bool prefixEmptyLine) {
    const char* safeText = (text != nullptr) ? text : "";
    const char* safePrefix = (prefix != nullptr) ? prefix : "";
    const size_t textLen = std::strlen(safeText);
    const size_t prefixLen = std::strlen(safePrefix);

    auto writeChunk = [&](const char* data, size_t len) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        size_t remaining = len;
        uint32_t retries = 0;

        while (remaining > 0) {
            const size_t sent = io.write(bytes, remaining);
            if (sent == 0) {
                ++retries;
                if (retries > 8) {
                    break;
                }
                delay(1);
                continue;
            }

            bytes += sent;
            remaining -= sent;
        }
    };

    if (textLen == 0) {
        if (prefixLen > 0 && prefixEmptyLine) {
            io.write('\r');
            writeChunk(safePrefix, prefixLen);
        }
        io.write('\r');
        io.write('\n');
        return;
    }

    io.write('\r');
    if (prefixLen > 0) {
        writeChunk(safePrefix, prefixLen);
    }

    bool lineOpen = true;
    for (size_t i = 0; i < textLen; ++i) {
        const char ch = safeText[i];
        if (ch == '\0') {
            break;
        }

        if (ch == '\r' || ch == '\n') {
            if (ch == '\r' && (i + 1) < textLen && safeText[i + 1] == '\n') {
                ++i;
            }

            io.write('\r');
            io.write('\n');
            lineOpen = false;

            if ((i + 1) < textLen) {
                io.write('\r');
                if (prefixLen > 0) {
                    writeChunk(safePrefix, prefixLen);
                }
                lineOpen = true;
            }
            continue;
        }

        io.write(ch);
    }

    if (lineOpen) {
        io.write('\r');
        io.write('\n');
    }
}

} // namespace

namespace ShellOutput {

const char* commandPrefix() {
    return kCommandPrefix;
}

std::string commandPrompt() {
    return std::string(kCommandPrefix);
}

void writeRawLine(ByteIO& io, const std::string& line) {
    writeRawLine(io, line.c_str());
}

void writeRawLine(ByteIO& io, const char* line) {
    writeTextWithPrefix(io, nullptr, line, false);
}

void writeLine(ByteIO& io, const std::string& line) {
    writeLine(io, line.c_str());
}

void writeLine(ByteIO& io, const char* line) {
    const char* safeLine = (line != nullptr) ? line : "";
    if (safeLine[0] == '\0') {
        writeTextWithPrefix(io, kOutputPrefix, safeLine, true);
        return;
    }

    if (hasVisualPrefix(safeLine)) {
        writeTextWithPrefix(io, nullptr, safeLine, false);
        return;
    }

    writeTextWithPrefix(io, kOutputPrefix, safeLine, true);
}

void writeLines(ByteIO& io, const char* text) {
    std::string normalized = normalizeNewlines(text);
    int start = 0;

    while (start <= static_cast<int>(normalized.size())) {
        const int newline = strcompat::indexOf(normalized, '\n', start);
        std::string line;

        if (newline < 0) {
            line = normalized.substr(static_cast<size_t>(start));
        } else {
            line = normalized.substr(static_cast<size_t>(start), static_cast<size_t>(newline - start));
        }

        strcompat::trim(line);
        if (!line.empty()) {
            writeLine(io, line.c_str());
        }

        if (newline < 0) {
            break;
        }

        start = newline + 1;
    }
}

void writeLines(ByteIO& io, const std::string& text) {
    writeLines(io, text.c_str());
}

void printTagged(ByteIO& io, const char* tag, const std::string& message) {
    const std::string safeTag = normalizeTag(tag);
    std::string line = message;
    strcompat::trim(line);

    if (line.empty()) {
        return;
    }

    if (strcompat::equalsIgnoreCase(safeTag, "espnow") && isEspNowStructuredLine(line)) {
        writeRawLine(io, line);
        return;
    }

    line = stripLeadingBracketTags(line);
    if (line.empty()) {
        return;
    }

    writeLine(io, line);
}

void printTagged(ByteIO& io, const char* tag, const char* message) {
    printTagged(io, tag, (message != nullptr) ? std::string(message) : std::string(""));
}

void printResponse(ByteIO& io, const std::string& response) {
    std::string text = response;
    strcompat::replaceAll(text, "\r\n", "\n");
    strcompat::replaceAll(text, "\r", "\n");
    strcompat::trim(text);
    if (text.empty()) {
        return;
    }

    int start = 0;
    while (start <= static_cast<int>(text.size())) {
        const int newline = strcompat::indexOf(text, '\n', start);
        std::string line;

        if (newline < 0) {
            line = text.substr(static_cast<size_t>(start));
        } else {
            line = text.substr(static_cast<size_t>(start), static_cast<size_t>(newline - start));
        }

        strcompat::trim(line);
        if (!line.empty()) {
            if (isEspNowStructuredLine(line)) {
                writeRawLine(io, line);
            } else {
                const std::string cleanLine = stripLeadingBracketTags(line);
                if (!cleanLine.empty()) {
                    writeLine(io, cleanLine);
                }
            }
        }

        if (newline < 0) {
            break;
        }
        start = newline + 1;
    }
}

std::string renderResponse(const std::string& response) {
    // The response may already carry ShellStyle SGR (TinyShell colours its own
    // framework messages, and ShellConfig::runLine strips it before handing the
    // text back -- but be defensive). Strip first so this function is the one
    // place that decides a terminal line's colour, keeping the "! " prefix and
    // the colour in agreement. Nothing upstream adds SGR in a no-colour build,
    // so skip the copy then.
    std::string text = shell_color_enabled() ? shell_strip_sgr(response) : response;
    strcompat::replaceAll(text, "\r\n", "\n");
    strcompat::replaceAll(text, "\r", "\n");
    strcompat::trim(text);

    std::string out;
    if (text.empty()) {
        return out;
    }

    int start = 0;
    while (start <= static_cast<int>(text.size())) {
        const int newline = strcompat::indexOf(text, '\n', start);
        std::string line;
        if (newline < 0) {
            line = text.substr(static_cast<size_t>(start));
        } else {
            line = text.substr(static_cast<size_t>(start), static_cast<size_t>(newline - start));
        }

        strcompat::trim(line);
        if (!line.empty()) {
            // Classify the raw line (before stripLeadingBracketTags eats a
            // leading "[help]" / ESP-NOW "[error]" tag the classifier keys on).
            const ShellMsgClass cls = shell_color_enabled()
                ? shell_classify_line(std::string_view(line.c_str(), line.size()))
                : ShellMsgClass::Output;
            if (isEspNowStructuredLine(line)) {
                appendLineWithPrefix(out, nullptr, line, cls);
            } else {
                const std::string cleanLine = stripLeadingBracketTags(line);
                if (!cleanLine.empty()) {
                    appendLineWithPrefix(out, hasVisualPrefix(cleanLine.c_str()) ? nullptr : kOutputPrefix,
                                         cleanLine, cls);
                }
            }
        }

        if (newline < 0) {
            break;
        }
        start = newline + 1;
    }

    return out;
}

} // namespace ShellOutput
