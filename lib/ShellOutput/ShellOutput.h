#pragma once

#include "compat.h"

#include <string>

namespace ShellOutput {

const char* commandPrefix();
std::string commandPrompt();

void writeRawLine(ByteIO& io, const std::string& line);
void writeRawLine(ByteIO& io, const char* line);

void writeLine(ByteIO& io, const std::string& line);
void writeLine(ByteIO& io, const char* line);
void writeLines(ByteIO& io, const std::string& text);
void writeLines(ByteIO& io, const char* text);

void printTagged(ByteIO& io, const char* tag, const std::string& message);
void printTagged(ByteIO& io, const char* tag, const char* message);

void printResponse(ByteIO& io, const std::string& response);

// Same per-line formatting printResponse() applies (CR+LF line endings, the
// "! " output prefix, leading-bracket-tag stripping, ESP-NOW structured-line
// passthrough), but returned as a string instead of written to a ByteIO --
// for the BTP terminal channel, whose "output" is chunked into TERMINAL_OUT
// frames rather than printed. Ends with a trailing CR+LF.
std::string renderResponse(const std::string& response);

} // namespace ShellOutput
